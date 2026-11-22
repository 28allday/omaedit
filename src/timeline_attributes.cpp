#include "timeline_p.h"

// A retimed clip's frame sampling (refused for a clip read as recorded).
static bool setSampling(edit::Producer &producer, int mode) {
    if (!FrameSampling::valid(mode) || (!producer.get("omavid:retime") && !producer.get_int("omavid:conform"))) return false;
    producer.set("omaedit:frameBlend", mode);
    return true;
}

bool Timeline::setClipPan(int index, double pan) {
    return setPanAt(0, index, pan);
}

bool Timeline::setConnectedPan(int index, double pan) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setPanAt(track, clip, pan);
}

bool Timeline::setClipEq(int index, const QString &preset) {
    return setEqAt(0, index, preset);
}

bool Timeline::setConnectedEq(int index, const QString &preset) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setEqAt(track, clip, preset);
}

// Per-clip volume in dB (Modify ▸ Adjust Volume) on a storyline clip.
bool Timeline::setClipVolume(int index, double db) {
    return setVolumeAt(0, index, db);
}

// The same adjustment on a connected-lane clip (detached audio, B-roll,
// audio-only connects).
bool Timeline::setConnectedVolume(int index, double db) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setVolumeAt(track, clip, db);
}

bool Timeline::setClipVolumeKf(int index, const QVariantList &kf) {
    return setVolumeKfAt(0, index, kf);
}

bool Timeline::setConnectedVolumeKf(int index, const QVariantList &kf) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setVolumeKfAt(track, clip, kf);
}

bool Timeline::setConnectedOpacity(int index, double alpha) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setOpacityAt(track, clip, alpha);
}

bool Timeline::setConnectedOpacityKf(int index, const QVariantList &kf) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setOpacityKfAt(track, clip, kf);
}

// Clip ▸ Detach Audio: one snapshot rebuild that marks the storyline
// clip video-only, plus a connected audio-only clip over the same frames
// (same source bounds, same speed — a retimed clip detaches at its rate).
// Any clip volume moves to the audio side. The caller records the whole
// thing as one undo step.

// ── Uniform per-clip adjustments (any track) ─────────────────────────────
bool Timeline::setRectAt(int track, int index, double x, double y,
                            double w, double h, double rotation, double anchorX,
                            double anchorY) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:rect"))
            cut->detach(*f);
    }
    if (w <= 0.0 || h <= 0.0) {
        w = 1.0;
        h = 1.0;
    }
    anchorX = qBound(0.0, anchorX, 1.0);
    anchorY = qBound(0.0, anchorY, 1.0);
    const bool centred = qFuzzyCompare(anchorX, 0.5) && qFuzzyCompare(anchorY, 0.5);
    const bool fullFrame =
        qFuzzyIsNull(x) && qFuzzyIsNull(y)
        && qFuzzyCompare(w, 1.0) && qFuzzyCompare(h, 1.0)
        && qFuzzyIsNull(rotation) && centred;
    bool ok = true;
    if (!fullFrame) {
        edit::Filter f(*m_profile, "affine");
        if (f.is_valid()) {
            // affine rotates about the rect's centre C. To turn about the
            // anchor P instead, hand it the rect moved by (P − C) − R(P − C)
            // (pixels: the frame is not square), R clockwise, y down.
            double ox = 0.0, oy = 0.0;
            if (!qFuzzyIsNull(rotation) && !centred) {
                const double fw = m_profile->width(), fh = m_profile->height();
                const double vx = (anchorX - 0.5) * w * fw;
                const double vy = (anchorY - 0.5) * h * fh;
                const double a = rotation * M_PI / 180.0;
                const double rx = vx * std::cos(a) - vy * std::sin(a);
                const double ry = vx * std::sin(a) + vy * std::cos(a);
                ox = (vx - rx) / fw;
                oy = (vy - ry) / fh;
            }
            const QString rect = QStringLiteral("%1%/%2%:%3%x%4%")
                                     .arg(QString::number((x + ox) * 100.0),
                                          QString::number((y + oy) * 100.0),
                                          QString::number(w * 100.0),
                                          QString::number(h * 100.0));
            f.set("transition.rect", rect.toUtf8().constData());
            f.set("transition.fill", 1);
            // Degrees, clockwise, about the (offset) rect's centre.
            f.set("transition.fix_rotate_x", rotation);
            f.set("omavid:rect",
                  QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
                      .arg(QString::number(x), QString::number(y),
                           QString::number(w), QString::number(h),
                           QString::number(rotation), QString::number(anchorX),
                           QString::number(anchorY))
                      .toUtf8().constData());
            cut->attach(f);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    }
    return ok;
}

// Crop each edge by a fraction of the clip's own picture via one qtcrop on
// the cut; all zero removes the filter. Works on V1 and track clips alike.
// Crop ranks before the transform (attributeRank), so on a picture in
// picture the fractions are of the scaled picture, not of the frame.
bool Timeline::setCropAt(int track, int index, double l, double r,
                            double t, double b, double feather, bool invert) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    l = qBound(0.0, l, 0.95);
    r = qBound(0.0, r, 0.95);
    t = qBound(0.0, t, 0.95);
    b = qBound(0.0, b, 0.95);
    feather = qBound(0.0, feather, 1.0);
    if (l + r > 0.95 || t + b > 0.95)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:crop"))
            cut->detach(*f);
    }
    const bool none = qFuzzyIsNull(l) && qFuzzyIsNull(r)
                      && qFuzzyIsNull(t) && qFuzzyIsNull(b);
    const bool soft = !none && (feather > 0.0 || invert);
    const QString marker = QStringLiteral("%1 %2 %3 %4 %5 %6")
                               .arg(QString::number(l), QString::number(r),
                                    QString::number(t), QString::number(b),
                                    QString::number(feather), QString::number(invert ? 1 : 0));
    bool ok = true;
    if (soft) {
        // frei0r.alphaspot: a rectangle (shape 0) centred at 1/2 with half
        // extents 3/4 (fractions of the picture), a soft band 6 wide, alpha
        // 7 outside → 8 inside. Invert swaps the two alphas. Operation 9
        // is min (2 of 0…4): the spot can only take alpha away, so what
        // is already transparent — a PNG's holes, or the empty frame
        // around a transform when the effect order puts crop after it —
        // stays transparent instead of turning into opaque black over the
        // tracks below. The cut keeps the same marker key so ordering,
        // bypass and paste treat it as the crop it is.
        edit::Filter f(*m_profile, "frei0r.alphaspot");
        if (f.is_valid()) {
            const double w = 1.0 - l - r, h = 1.0 - t - b;
            f.set("0", 0.0);
            f.set("1", l + w / 2.0);
            f.set("2", t + h / 2.0);
            f.set("3", w / 2.0);
            f.set("4", h / 2.0);
            f.set("5", 0.5);
            f.set("6", feather);
            f.set("7", invert ? 1.0 : 0.0);
            f.set("8", invert ? 0.0 : 1.0);
            f.set("9", 0.5);
            f.set("omavid:crop", marker.toUtf8().constData());
            f.set("omavid:cropAlpha", 1);
            cut->attach(f);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    } else if (!none) {
        edit::Filter f(*m_profile, "qtcrop");
        if (f.is_valid()) {
            const QString rect = QStringLiteral("%1%/%2%:%3%x%4%")
                                     .arg(QString::number(l * 100.0),
                                          QString::number(t * 100.0),
                                          QString::number((1.0 - l - r) * 100.0),
                                          QString::number((1.0 - t - b) * 100.0));
            f.set("rect", rect.toUtf8().constData());
            f.set("color", "#00000000"); // outside the crop: transparent
            f.set("omavid:crop", marker.toUtf8().constData());
            cut->attach(f);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    }
    return ok;
}

bool Timeline::setClipCrop(int index, double l, double r, double t, double b,
                              double feather, bool invert) {
    return setCropAt(0, index, l, r, t, b, feather, invert);
}

bool Timeline::setConnectedCrop(int index, double l, double r, double t,
                                   double b, double feather, bool invert) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    return setCropAt(track, clip, l, r, t, b, feather, invert);
}

bool Timeline::setConnectedAnchor(int index, double anchorX, double anchorY) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, clip);
    if (!info)
        return false;
    double x = 0, y = 0, w = 1, h = 1, rot = 0;
    connectedRect(info->cut, &x, &y, &w, &h, &rot);
    return setRectAt(track, clip, x, y, w, h, rot, anchorX, anchorY);
}

bool Timeline::setFrameBlendAt(int track, int index, int mode) {
    if(!FrameSampling::valid(mode) || trackLocked(track))return false;
    const auto entry=entryMapOn(track,index);
    if(entry.isEmpty())return false;
    const bool mapped=entry.contains(ClipKey::retime);
    if(!m_restoring && entry.value(ClipKey::frameBlend).toInt()==mode)return false;
    if((mapped || FrameSampling::explicitPictures(mode)
        || FrameSampling::explicitPictures(entry.value(ClipKey::frameBlend).toInt())) && !m_restoring) {
        // Split cuts can share a chain. Rebuild through the normal snapshot
        // path to give each clip its own sampling state, including transitions.
        const auto before=entries();auto changed=before;
        const int at=snapshotIndexOn(changed,track,index,nullptr);if(at<0)return false;
        auto next=entry;next[ClipKey::frameBlend]=mode;changed[at]=next;
        if(restore(changed))return true;
        restore(before);return false;
    }
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Filter tag(*m_profile, "brightness"); // identity carrier
    if(mode>0 && !tag.is_valid())return false;
    // An offline placeholder has no recorded pictures to resample. Retain the
    // cut's saved choice so relinking reconstructs the correct real producer.
    const int engineMode=entry.contains(ClipKey::sourceOrigin) && !mapped && mode==FrameSampling::Original
        ? FrameSampling::Nearest : mode;
    if(!entry.value(ClipKey::offline).toBool()
        && (mapped || (info->producer && info->producer->get_int("omavid:conform")))
        && (!info->producer || !setSampling(*info->producer,engineMode)))return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:frameBlend"))
            cut->detach(*f);
    }
    if (mode > 0) {
        tag.set("level", 1);
        tag.set("omavid:frameBlend", mode);
        cut->attach(tag);
    }
    return true;
}

bool Timeline::setAdjustAt(int track, int index, bool adjust) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:adjust"))
            cut->detach(*f);
    }
    if (adjust) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:adjust", 1);
        cut->attach(tag);
    }
    return true;
}

QString Timeline::generatorResource(const QString &kind, const QString &color) {
    if (kind == QStringLiteral("adjust"))
        return QStringLiteral("color:#00000000");
    if (kind == QStringLiteral("bars"))
        return QStringLiteral("frei0r.test_pat_B");
    if (kind == QStringLiteral("noise"))
        return QStringLiteral("noise");
    if (kind == QStringLiteral("count"))
        return QStringLiteral("count");
    if (kind == QStringLiteral("solid")) {
        const QString c = color.trimmed();
        const bool hex = (c.size() == 7 || c.size() == 9) && c.startsWith(QLatin1Char('#'));
        return QStringLiteral("color:") + (hex ? c : QStringLiteral("#808080"));
    }
    return QString();
}

QString Timeline::generatorKindOf(const QString &path) {
    if (path == QStringLiteral("frei0r.test_pat_B"))
        return QStringLiteral("bars");
    if (path == QStringLiteral("noise"))
        return QStringLiteral("noise");
    if (path == QStringLiteral("count"))
        return QStringLiteral("count");
    // A solid is a colour clip that is neither a gap's black nor a title's
    // transparent (those carry their own markers and never reach here), nor
    // the black of a multicam angle with no footage.
    if (path.startsWith(QStringLiteral("color:#")) && path != QStringLiteral("color:black") && path != noFootagePath())
        return QStringLiteral("solid");
    return QString();
}

bool Timeline::setConnectedRotation(int index, double degrees) {
    int track = -1, clip = -1;
    if (!connectedAddress(index, &track, &clip))
        return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, clip);
    if (!info)
        return false;
    double x = 0, y = 0, w = 1, h = 1, rot = 0, ax = 0.5, ay = 0.5;
    connectedRect(info->cut, &x, &y, &w, &h, &rot, &ax, &ay);
    return setRectAt(track, clip, x, y, w, h, degrees, ax, ay);
}
bool Timeline::setColorAt(int track, int index, double brightness,
                             double contrast, double saturation) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    brightness = qBound(-1.0, brightness, 1.0);
    contrast = qBound(0.0, contrast, 2.0);
    saturation = qBound(0.0, saturation, 3.0);
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:color"))
            cut->detach(*f);
    }
    const bool neutral = qFuzzyIsNull(brightness)
                         && qFuzzyCompare(contrast, 1.0)
                         && qFuzzyCompare(saturation, 1.0);
    bool ok = true;
    if (!neutral) {
        edit::Filter eq(*m_profile, "avfilter.eq");
        if (eq.is_valid()) {
            eq.set("av.brightness", brightness);
            eq.set("av.contrast", contrast);
            eq.set("av.saturation", saturation);
            eq.set("omavid:color",
                   QStringLiteral("%1 %2 %3")
                       .arg(QString::number(brightness),
                            QString::number(contrast),
                            QString::number(saturation))
                       .toUtf8().constData());
            cut->attach(eq);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    }
    return ok;
}
bool Timeline::setDisabledAt(int track, int index, bool disabled) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return setAudioEntry(track,index,{{"disabled",disabled}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:disabled"))
            cut->detach(*f);
    }
    if (disabled) {
        edit::Filter dark(*m_profile, "brightness");
        edit::Filter mute(*m_profile, "volume");
        if (!dark.is_valid() || !mute.is_valid()) {
                    return false;
        }
        dark.set("level", 0);
        dark.set("omavid:disabled", 1);
        cut->attach(dark);
        mute.set("level", -60); // dB — silence
        mute.set("omavid:disabled", 1);
        cut->attach(mute);
    }
    return true;
}

bool Timeline::setCategoryAt(int track, int index, const QString &category) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return setAudioEntry(track,index,{{"category",category}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:category"))
            cut->detach(*f);
    }
    if (!category.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:category", category.toUtf8().constData());
        cut->attach(tag);
    }
    return true;
}

bool Timeline::setLabelAt(int track, int index, const QString &label) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return setAudioEntry(track,index,{{"label",label}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:label"))
            cut->detach(*f);
    }
    if (!label.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:label", label.toUtf8().constData());
        cut->attach(tag);
    }
    return true;
}

bool Timeline::setLinkAt(int track, int index, const QString &link) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return setAudioEntry(track,index,{{"link",link}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:link"))
            cut->detach(*f);
    }
    if (!link.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:link", link.toUtf8().constData());
        cut->attach(tag);
    }
    return true;
}

bool Timeline::setClipMarkersAt(int track, int index, const QVariantList &markers) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return setAudioEntry(track,index,{{"markers",markers}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    std::vector<std::unique_ptr<edit::Filter>> previous;
    for (int i = 0; i < cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> filter(cut->filter(i));
        if (filter && filter->is_valid() && filter->get("omavid:markers"))
            previous.push_back(std::move(filter));
    }
    // Attach the replacement before removing the old carrier. A missing service
    // or failed attachment must leave the existing marker metadata intact.
    if (!markers.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        const QByteArray json = QJsonDocument(QJsonArray::fromVariantList(markers))
                                    .toJson(QJsonDocument::Compact);
        tag.set("omavid:markers", json.constData());
        if (cut->attach(tag) != 0)
            return false;
    }
    for (const auto &filter : previous)
        cut->detach(*filter);
    return true;
}

bool Timeline::setOriginalAt(int track, int index, const QVariantMap &original) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:original"))
            cut->detach(*f);
    }
    if (!original.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(original))
                                    .toJson(QJsonDocument::Compact);
        tag.set("omavid:original", json.constData());
        cut->attach(tag);
    }
    return true;
}

// Blend / lens / stabilisation from an entries() map, onto a rebuilt or
// pasted clip. A stabilisation whose analysis file is gone is dropped —
// vidstab would otherwise try to analyse on the fly during playback.
void Timeline::applyLooks(int track, int index, const ClipEntry &e) {
    if (e.videoEffects) {
        auto info = clipInfoAt(track, index);
        if (info && info->cut && VideoEffects::apply(*m_profile, info->cut, *e.videoEffects, true)) {
            reorderAttributeFilters(info->cut);
        }
    }
    if (e.audioCurve) setAudioFadeCurveAt(track, index, *e.audioCurve);
    if (e.grade)
        setGradeAt(track, index, *e.grade);
    if (render::isScalingKey(ClipEntry::orDefault(e.scaling)))
        setScalingAt(track, index, *e.scaling);
    const int blend = ClipEntry::orDefault(e.blend);
    if (blend > 0 && track >= 1) // V1 composites over black: Multiply = black
        setBlendAt(track, index, blend);
    if (e.lensK1)
        setLensAt(track, index, *e.lensK1, ClipEntry::orDefault(e.lensK2), e.lensCx.value_or(0.5), e.lensCy.value_or(0.5));
    const QVariantMap stab = ClipEntry::orDefault(e.stab);
    const QString results = stab.value(QStringLiteral("results")).toString();
    if (!results.isEmpty() && QFileInfo::exists(results))
        setStabAt(track, index, results, stab.value(QStringLiteral("shakiness"), 4).toInt(),
                  stab.value(QStringLiteral("smoothing"), 15).toInt());
}

bool Timeline::setScalingAt(int track, int index, const QString &scaling) {
    if (!scaling.isEmpty() && !render::isScalingKey(scaling))
        return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:scaling"))
            cut->detach(*f);
    }
    if (!scaling.isEmpty()) {
        edit::Filter tag(*m_profile, "brightness"); // identity carrier; the renderers read it
        if (!tag.is_valid())
            return false;
        tag.set("level", 1);
        tag.set("omavid:scaling", scaling.toUtf8().constData());
        cut->attach(tag);
    }
    return true;
}

bool Timeline::setBlendAt(int track, int index, int mode) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:blend"))
            cut->detach(*f);
    }
    if (mode <= 0)
        return true;
    edit::Filter f(*m_profile, "qtblend_mode");
    if (!f.is_valid())
        return false;
    f.set("mode", mode);
    f.set("omavid:blend", mode);
    cut->attach(f);
    reorderAttributeFilters(cut);
    return true;
}

bool Timeline::setVideoEffectsAt(int track, int index, const QVariantList &effects, QString *error) {
    if (error) error->clear();
    const auto entry = entryMapOn(track, index);
    if (trackLocked(track) || entry.isEmpty() || entry.value(ClipKey::audioOnly).toBool() || entry.value(ClipKey::gap).toBool()) {
        if (error) *error = QCoreApplication::translate("Timeline", "Select a picture clip on an unlocked track to edit video effects.");
        return false;
    }
    auto info = clipInfoAt(track, index);
    if (!info || !info->cut || effects == VideoEffects::read(info->cut)) return false;
    if (!VideoEffects::apply(*m_profile, info->cut, effects, false, error)) return false;
    reorderAttributeFilters(info->cut);
    return true;
}

bool Timeline::editVideoEffectAt(int track, int index, const QString &action,
                                   const QString &uid, const QVariantMap &values, QString *error) {
    if (error) error->clear();
    auto fail = [&](const char *message) { if (error) *error = QCoreApplication::translate("Timeline", message); return false; };
    auto stack = entryMapOn(track, index).value(ClipKey::videoEffects).toList();
    QStringList allowed;
    if (action == "add") allowed = {"id", "params"};
    else if (action == "update") allowed = {"params", "bypass", "keys", "mask", "maskKeys"};
    else if (action == "move") allowed = {"direction"};
    else if (action != "remove" && action != "reset") return fail(QT_TRANSLATE_NOOP("Timeline", "Unknown video effect action."));
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!allowed.contains(it.key())) return fail(QT_TRANSLATE_NOOP("Timeline", "Unknown video effect setting."));
    if (values.contains("params") && values.value("params").metaType().id() != QMetaType::QVariantMap)
        return fail(QT_TRANSLATE_NOOP("Timeline", "Effect parameters must be an object."));
    if (values.contains("keys") && values.value("keys").metaType().id() != QMetaType::QVariantMap)
        return fail(QT_TRANSLATE_NOOP("Timeline", "Effect keyframes must be an object."));
    if (values.contains("maskKeys") && values.value("maskKeys").metaType().id() != QMetaType::QVariantMap)
        return fail(QT_TRANSLATE_NOOP("Timeline", "Effect keyframes must be an object."));
    if (values.contains("mask") && values.value("mask").metaType().id() != QMetaType::QVariantMap)
        return fail(QT_TRANSLATE_NOOP("Timeline", "An effect's mask must be an object."));
    if (values.contains("bypass") && values.value("bypass").metaType().id() != QMetaType::Bool)
        return fail(QT_TRANSLATE_NOOP("Timeline", "Effect bypass must be true or false."));
    if (action == "add") {
        const auto id = values.value("id").toString();
        auto params = VideoEffects::defaults(id); const auto supplied = values.value("params").toMap();
        for (auto it = supplied.begin(); it != supplied.end(); ++it) params[it.key()] = it.value();
        stack.append(QVariantMap{{"id", id}, {"uid", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"params", params}, {"bypass", false}});
    } else {
        int at = -1;
        for (int i = 0; i < stack.size(); ++i) if (stack[i].toMap().value("uid") == uid && !uid.isEmpty()) { at = i; break; }
        if (at < 0) return fail(QT_TRANSLATE_NOOP("Timeline", "The selected video effect no longer exists."));
        auto state = stack[at].toMap();
        if (action == "remove") stack.removeAt(at);
        else if (action == "move") {
            const double direction = values.value("direction").toDouble();
            if (direction != -1 && direction != 1) return fail(QT_TRANSLATE_NOOP("Timeline", "Choose the previous or next effect position."));
            const int to = at + int(direction);
            if (to < 0 || to >= stack.size()) return false;
            stack.move(at, to);
        } else {
            auto params = action == "reset" ? VideoEffects::defaults(state.value("id").toString()) : state.value("params").toMap();
            const auto patch = values.value("params").toMap();
            for (auto it = patch.begin(); it != patch.end(); ++it) params[it.key()] = it.value();
            state["params"] = params;
            // Keys come a control at a time: a list replaces that control's,
            // an empty one ends its animation. Reset ends them all.
            auto keys = action == "reset" ? QVariantMap{} : state.value("keys").toMap();
            const auto keyPatch = values.value("keys").toMap();
            for (auto it = keyPatch.begin(); it != keyPatch.end(); ++it) {
                if (it.value().toList().isEmpty()) keys.remove(it.key());
                else keys[it.key()] = it.value();
            }
            if (keys.isEmpty()) state.remove("keys"); else state["keys"] = keys;
            // A mask comes whole; an empty one takes it off.
            if (values.contains("mask")) {
                if (values.value("mask").toMap().isEmpty()) state.remove("mask"); else state["mask"] = values.value("mask");
            }
            // The mask's own keys, a field at a time like the controls'. A
            // mask taken off takes them with it; a polygon keeps its feather's and its corners'.
            auto maskKeys = state.value("maskKeys").toMap();
            const auto maskKeyPatch = values.value("maskKeys").toMap();
            for (auto it = maskKeyPatch.begin(); it != maskKeyPatch.end(); ++it) {
                if (it.value().toList().isEmpty()) maskKeys.remove(it.key());
                else maskKeys[it.key()] = it.value();
            }
            if (!state.contains("mask")) maskKeys.clear();
            else if (state.value("mask").toMap().value("shape") == "polygon") {
                for (const char *field : {"x", "y", "w", "h", "rotation"}) maskKeys.remove(QLatin1String(field));
                // Its corners' keys go when a corner is added or taken away.
                const auto corners = maskKeys.value("points").toList();
                if (values.contains("mask") && !maskKeyPatch.contains("points") && !corners.isEmpty()
                    && corners.first().toMap().value("value").toList().size() != state.value("mask").toMap().value("points").toList().size())
                    maskKeys.remove("points");
            } else if (!maskKeyPatch.contains("points")) maskKeys.remove("points");
            if (maskKeys.isEmpty()) state.remove("maskKeys"); else state["maskKeys"] = maskKeys;
            if (values.contains("bypass")) state["bypass"] = values.value("bypass");
            stack[at] = state;
        }
    }
    return setVideoEffectsAt(track, index, stack, error);
}

bool Timeline::setLensAt(int track, int index, double k1, double k2, double cx, double cy) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:lens"))
            cut->detach(*f);
    }
    k1 = qBound(-1.0, k1, 1.0);
    k2 = qBound(-1.0, k2, 1.0);
    cx = qBound(0.0, cx, 1.0);
    cy = qBound(0.0, cy, 1.0);
    if (qFuzzyIsNull(k1) && qFuzzyIsNull(k2))
        return true;
    edit::Filter f(*m_profile, "avfilter.lenscorrection");
    if (!f.is_valid())
        return false;
    f.set("av.k1", k1);
    f.set("av.k2", k2);
    f.set("av.cx", cx);
    f.set("av.cy", cy);
    f.set("omavid:lens", 1);
    cut->attach(f);
    reorderAttributeFilters(cut);
    return true;
}

bool Timeline::setStabAt(int track, int index, const QString &results, int shakiness,
                            int smoothing) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:stab"))
            cut->detach(*f);
    }
    if (results.isEmpty())
        return true;
    edit::Filter f(*m_profile, "vidstab");
    if (!f.is_valid())
        return false;
    f.set("results", results.toUtf8().constData());
    f.set("smoothing", qBound(0, smoothing, 100));
    f.set("shakiness", qBound(1, shakiness, 10));
    f.set("omavid:stab", 1);
    cut->attach(f);
    reorderAttributeFilters(cut);
    return true;
}

QString Timeline::newLinkId() const {
    return QString::number(QRandomGenerator::global()->generate64(), 36);
}

bool Timeline::setFlipAt(int track, int index, bool h, bool v) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:flip"))
            cut->detach(*f);
    }
    bool ok = true;
    if (h) {
        edit::Filter f(*m_profile, "avfilter.hflip");
        if (f.is_valid()) {
            f.set("omavid:flip", "h");
            cut->attach(f);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    }
    if (v) {
        edit::Filter f(*m_profile, "avfilter.vflip");
        if (f.is_valid()) {
            f.set("omavid:flip", "v");
            cut->attach(f);
            reorderAttributeFilters(cut);
        } else {
            ok = false;
        }
    }
    return ok;
}

bool Timeline::setFreezeAt(int track, int index, int sourceFrame, bool on) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:freeze"))
            cut->detach(*f);
    }
    if (on) {
        edit::Filter f(*m_profile, "freeze");
        if (!f.is_valid())
            return false;
        // filter_freeze.c holds frame + the in point of the frame's original
        // producer. That is the cut for a plain producer, but frames from a
        // chain (every link-based clip) name the chain's source, whose in is
        // its own: count from whichever it will see.
        const edit::Producer parent = cut->parent();
        const int origin = parent.get("omavid:retime") || parent.get_int("omavid:conform") ? 0 : info->frame_in;
        f.set("frame", qMax(0, sourceFrame - origin));
        f.set("freeze_before", 0);
        f.set("freeze_after", 0);
        f.set("omavid:freeze", sourceFrame);
        cut->attach(f);
        reorderAttributeFilters(cut);
    }
    return true;
}

int Timeline::sourceFrameCount(const QString &path) {
    if (path.isEmpty())
        return 0;
    if (m_profile) {
        // A file: its length on the timeline's clock.
        if (QFileInfo(path).isFile() && !ProjectFile::isProject(path)) {
            const media::Probe probe = media::probeCached(path);
            return probe.valid ? media::framesOn(probe, m_profile->fps()) : 0;
        }
        edit::Producer p(*m_profile, AnimatedWebP::resource(path).toUtf8().constData());
        return p.is_valid() ? p.get_length() : 0;
    }
    // Before the first clip there is no profile yet: count in the one that
    // clip is about to lock (a counting leader starting an empty timeline
    // sized its range from 0 frames and counted from the wrong end).
    if (!QFileInfo(path).isFile()) return MediaPlayer::kStillFrames;
    const media::Probe probe = media::probeCached(path);
    if (!probe.valid) return 0;
    edit::Profile profile;
    profileFromProbe(profile, probe);
    applyProjectProfile(profile);
    return media::framesOn(probe, profile.fps());
}

bool Timeline::freezeFrameOn(int track, int index, int frame, int length) {
    if (trackLocked(track) || length < 1)
        return false;
    QVariantList clips = entries();
    const QVariantList pristine = clips;
    int at = snapshotIndexOn(clips, track, index, nullptr);
    if (at < 0)
        return false;
    QVariantMap head = clips.at(at).toMap();
    if (head.value(ClipKey::gap).toBool()
        || head.value(ClipKey::audioOnly).toBool()
        || !head.value(ClipKey::title).toString().isEmpty()
        || !head.value(ClipKey::compoundJson).toString().isEmpty()
        || head.value(ClipKey::mixOut).toInt() > 0
        || head.value(ClipKey::mixIn).toInt() > 0)
        return false;
    const int start = head.value(ClipKey::start).toInt();
    const int frames = head.value(ClipKey::frames).toInt();
    const int rel = frame - start;
    if (rel < 0 || rel >= frames)
        return false;
    const QString path = head.value(ClipKey::path).toString();
    const int in = head.value(ClipKey::in).toInt();
    const int out = head.value(ClipKey::out).toInt();
    const auto camera=multicamDefinition(head);const bool nativeMulticam=camera.value("version").toInt()==2;
    const double speed = qAbs(head.value(ClipKey::speed, 1.0).toDouble());
    // `in`/`out` of a retimed entry count timewarp frames (see RetimeEdit), so
    // the frame under the playhead is timewarp frame in + rel, and the file
    // frame it shows is that times the speed.
    // A reversed clip mirrors its timewarp frames (as its markers do): frame t
    // shows what forward frame length-1-t would. Other reversed timings are
    // refused rather than frozen on the wrong picture.
    int timewarpFrame = in + rel;
    if (head.value(ClipKey::speed, 1.0).toDouble() < 0.0 && !nativeMulticam) {
        const int timewarpLength = head.value(ClipKey::length).toInt();
        if (head.contains(ClipKey::sourceOrigin) || head.contains(ClipKey::retime) || timewarpLength < 1)
            return false;
        timewarpFrame = timewarpLength - 1 - timewarpFrame;
    }
    const int source = qRound(timewarpFrame * (speed > 0.0 ? speed : 1.0));
    QVariantMap held;
    if(nativeMulticam) {
        held=heldMulticamTiming(head,camera,fps(),head.contains(ClipKey::freeze)?head.value(ClipKey::freeze).toInt():in+rel,length);
        if(held.isEmpty())return false;
    }
    const int fileFrames = nativeMulticam?length:sourceFrameCount(path);
    if (fileFrames < 1)return false;
    // The still needs a real cut under it: run forward from the frame, or
    // back up to it near the file's end, or take the whole short file.
    int cutIn = nativeMulticam?0:source;
    if (!nativeMulticam && source + length - 1 >= fileFrames) {
        if (source - length + 1 >= 0) {
            cutIn = source - length + 1;
        } else {
            cutIn = 0;
            length = fileFrames;
        }
    }
    QVariantMap still = head;
    for (const char *key : {"mixIn", "mixOut", "mixStyle", "mixAlign", "mixExtA", "mixExtB",
                            "fadeIn", "fadeOut", "volume",
                            "volumeKf", "pan", "eq", "name", "compoundJson", "mcam",
                            "mcamAngle", "gapBefore", "speed", "audioOnly"})
        if(!nativeMulticam || (qstrcmp(key,"mcam") && qstrcmp(key,"mcamAngle")))still.remove(QLatin1String(key));
    still.insert(ClipKey::in, cutIn);
    still.insert(ClipKey::out, cutIn + length - 1);
    still.insert(ClipKey::frames, length);
    still.insert(ClipKey::start, frame);
    still.insert(ClipKey::videoOnly, true);
    if(nativeMulticam) {
        still[ClipKey::retime]=held;still[ClipKey::speed]=1.;still[ClipKey::length]=length;still.remove(ClipKey::freeze);still.remove(ClipKey::sourceTiming);
        if(head.contains(ClipKey::sourceOrigin))still[ClipKey::sourceOrigin]=0.;
        if(!head.contains(ClipKey::retime) && head.contains(ClipKey::sourceOrigin) && head.value(ClipKey::frameBlend).toInt()==FrameSampling::Original)
            still[ClipKey::frameBlend]=FrameSampling::Nearest;
        QVariantList markers;
        for(const auto &value:head.value(ClipKey::markers).toList()) {
            auto marker=value.toMap();if(marker.value("frame").toInt()==in+rel){marker["frame"]=0;markers<<marker;}
        }
        still[ClipKey::markers]=markers;
        if(head.contains(ClipKey::clipId))still[ClipKey::clipId]=QUuid::createUuid().toString(QUuid::WithoutBraces);
    } else still.insert(ClipKey::freeze, source);
    // Shift what follows: this lane's later clips, or (V1) the
    // auto-selected tracks' clips from the frame on — an insert's ripple.
    for (int i = 0; i < clips.size(); ++i) {
        if (i == at)
            continue;
        QVariantMap m = clips.at(i).toMap();
        const int t = m.value(ClipKey::track).toInt();
        if (t == 0)
            continue; // the storyline's order carries its own ripple
        const bool rides = track == 0 ? (t < trackCount() && m_tracks[size_t(t)].autoSelect
                                         && !m_tracks[size_t(t)].locked)
                                      : t == track;
        if (rides && m.value(ClipKey::start).toInt() >= frame) {
            if (!moveEntryStart(m, m.value(ClipKey::start).toInt() + length, *m_profile))
                return false;
            clips[i] = m;
        }
    }
    if (rel == 0) {
        QVariantMap after = head;
        after.remove(ClipKey::gapBefore);
        if (track != 0)
            after.insert(ClipKey::start, start + length);
        if (head.contains(ClipKey::gapBefore)) // the blank stays before the still
            still.insert(ClipKey::gapBefore, head.value(ClipKey::gapBefore));
        still.remove(ClipKey::link); // the sound stays with the clip after it
        clips[at] = still;
        clips.insert(at + 1, after);
    } else {
        const int headSource = qMax(1, rel); // timewarp frames map 1:1 onto timeline frames
        QVariantMap tail = head;
        head.insert(ClipKey::frames, rel);
        head.insert(ClipKey::out, qMin(out, in + headSource - 1));
        head.insert(ClipKey::fadeOut, 0);
        tail.insert(ClipKey::start, frame + length);
        tail.insert(ClipKey::frames, frames - rel);
        tail.insert(ClipKey::in, qMin(out, in + headSource));
        tail.insert(ClipKey::fadeIn, 0);
        tail.remove(ClipKey::gapBefore);
        // Keyframes are clip-relative: the tail's re-base to its own first
        // frame, the way a blade divides them.
        auto divideKeys = [rel](QVariantMap *left, QVariantMap *right, const char *key, const char *value) {
            const QString k = QLatin1String(key);
            const QVariantList keys = left->value(k).toList();
            if (keys.isEmpty())
                return;
            QVariantList l, r;
            splitKeys(keys, value, rel, &l, &r);
            left->insert(k, l);
            right->insert(k, r);
        };
        divideKeys(&head, &tail, "volumeKf", "db");
        divideKeys(&head, &tail, "opacityKf", "alpha");
        if (!head.value(ClipKey::editKeys).toMap().isEmpty())
            tail.insert(ClipKey::editKeys,
                        EditEngine::shiftedKeys(head.value(ClipKey::editKeys).toMap(), rel, frames - rel));
        if (head.contains(ClipKey::videoEffects))
            tail.insert(ClipKey::videoEffects, EditEngine::shiftedEffects(head.value(ClipKey::videoEffects).toList(), rel));
        // Linked sound is cut at the same frame. Head and tail each keep a
        // pair of their own and nothing plays under the still; sound that
        // cannot be cut (locked, or not under the frame) stays with the head.
        still.remove(ClipKey::link);
        tail.remove(ClipKey::link);
        const QString link = head.value(ClipKey::link).toString();
        // (A multicam clip with a sound source of its own whose sound the
        // editor removed: the part after the still has none either, and says
        // so as the picture did, by a link of its own to no sound.)
        bool soundless = !link.isEmpty() && !fixedSoundOf(head).isEmpty();
        for (int i = 0; i < clips.size() && soundless; ++i)
            soundless = i == at || !clips.at(i).toMap().value(ClipKey::audioOnly).toBool() || clips.at(i).toMap().value(ClipKey::link).toString() != link;
        if (soundless)
            tail.insert(ClipKey::link, newLinkId());
        for (int i = 0; i < clips.size() && !link.isEmpty(); ++i) {
            QVariantMap aHead = clips.at(i).toMap();
            if (i == at || !aHead.value(ClipKey::audioOnly).toBool()
                || aHead.value(ClipKey::link).toString() != link)
                continue;
            const int aFrames = aHead.value(ClipKey::frames).toInt();
            const int aRel = frame - aHead.value(ClipKey::start).toInt();
            if (aRel < 1 || aRel >= aFrames || trackLocked(aHead.value(ClipKey::track).toInt()))
                break;
            const int aIn = aHead.value(ClipKey::in).toInt();
            const int aOut = aHead.value(ClipKey::out).toInt();
            QVariantMap aTail = aHead;
            aHead.insert(ClipKey::frames, aRel);
            aHead.insert(ClipKey::out, qMin(aOut, aIn + aRel - 1));
            aHead.insert(ClipKey::fadeOut, 0);
            aTail.insert(ClipKey::start, frame + length);
            aTail.insert(ClipKey::frames, aFrames - aRel);
            aTail.insert(ClipKey::in, qMin(aOut, aIn + aRel));
            aTail.insert(ClipKey::fadeIn, 0);
            const QVariantList keys = aHead.value(ClipKey::volumeKf).toList();
            if (!keys.isEmpty()) {
                QVariantList l, r;
                splitKeys(keys, "db", aRel, &l, &r);
                aHead.insert(ClipKey::volumeKf, l);
                aTail.insert(ClipKey::volumeKf, r);
            }
            const QString fresh = newLinkId();
            aTail.insert(ClipKey::link, fresh);
            tail.insert(ClipKey::link, fresh);
            clips[i] = aHead;
            clips.insert(i + 1, aTail);
            if (i < at)
                ++at;
            break;
        }
        clips[at] = head;
        clips.insert(at + 1, still);
        clips.insert(at + 2, tail);
    }
    return rebuild(clips, pristine);
}

bool Timeline::setAudioMapAt(int track, int index, const QString &mode) {
    static const QStringList known = {QString(), QStringLiteral("mono"), QStringLiteral("swap"),
                                      QStringLiteral("left"), QStringLiteral("right")};
    if (!known.contains(mode))
        return false;
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:audioMap"))
            cut->detach(*f);
    }
    if (mode.isEmpty())
        return true;
    const char *id = mode == QStringLiteral("mono") ? "mono"
                     : mode == QStringLiteral("swap") ? "channelswap" : "channelcopy";
    edit::Filter f(*m_profile, id);
    if (!f.is_valid())
        return false;
    if (mode == QStringLiteral("mono")) {
        f.set("channels", 2); // fold to the centre, still a stereo stream
    } else if (mode == QStringLiteral("swap")) {
        f.set("from", 0);
        f.set("to", 1);
        f.set("swap", 1);
    } else { // one side onto both
        f.set("from", mode == QStringLiteral("left") ? 0 : 1);
        f.set("to", mode == QStringLiteral("left") ? 1 : 0);
    }
    f.set("omavid:audioMap", mode.toUtf8().constData());
    cut->attach(f);
    return true;
}

bool Timeline::setNormalizeAt(int track, int index, bool on) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return false;
    edit::Producer *cut = info->cut;
    for (int i = cut->filter_count() - 1; i >= 0; --i) {
        std::unique_ptr<edit::Filter> f(cut->filter(i));
        if (f && f->is_valid() && f->get("omavid:normalize"))
            cut->detach(*f);
    }
    if (!on)
        return true;
    // Single-pass loudness riding: EBU R128 programme level, a 3 s window,
    // ±15 dB of reach — no analysis pass, so it works on a live timeline.
    edit::Filter f(*m_profile, "dynamic_loudness");
    if (!f.is_valid())
        return false;
    f.set("target_loudness", -23.0);
    f.set("window", 3.0);
    f.set("max_gain", 15.0);
    f.set("min_gain", -15.0);
    f.set("omavid:normalize", 1);
    cut->attach(f);
    return true;
}

bool Timeline::setVolumeAt(int track, int index, double db) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return std::isfinite(db) && setAudioEntry(track,index,{{"volume",qBound(-60.,db,12.)},{"volumeKf",QVariantList{}}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyVolumeDb(*m_profile, info->cut, db);
}

bool Timeline::setVolumeKfAt(int track, int index, const QVariantList &kf) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyVolumeKf(*m_profile, info->cut, kf, info->frame_in,
                         info->frame_out);
}

bool Timeline::setPanAt(int track, int index, double pan) {
    if (track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty())
        return std::isfinite(pan) && setAudioEntry(track,index,{{"pan",qBound(-1.,pan,1.)}});
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyPanValue(*m_profile, info->cut, pan);
}

bool Timeline::setEqAt(int track, int index, const QString &preset) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyEqPreset(*m_profile, info->cut, preset);
}

bool Timeline::setOpacityAt(int track, int index, double alpha) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyOpacityValue(*m_profile, info->cut, alpha);
}

bool Timeline::setOpacityKfAt(int track, int index, const QVariantList &kf) {
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info)
        return false;
    return applyOpacityKf(*m_profile, info->cut, kf, info->frame_in,
                          info->frame_out);
}

QStringList Timeline::bypassableKeys() {
    return {QStringLiteral("color"), QStringLiteral("crop"), QStringLiteral("flip"),
            QStringLiteral("opacity"), QStringLiteral("rect"), QStringLiteral("level"),
            QStringLiteral("eq"), QStringLiteral("audioMap"), QStringLiteral("normalize"),
            QStringLiteral("blend"), QStringLiteral("lens"), QStringLiteral("stab"),
            QStringLiteral("grade")};
}

bool Timeline::setBypassAt(int track, int index, const QString &key, bool bypass) {
    if(track>0 && track<trackCount() && !m_tracks[size_t(track)].audioEntries.isEmpty()) {
        if(key!="level" || index<0 || index>=trackClipCount(track))return false;
        auto keys=m_tracks[size_t(track)].audioEntries[index].toMap().value("bypass").toStringList();
        keys.removeAll(key);if(bypass)keys.append(key);
        return setAudioEntry(track,index,{{"bypass",keys}});
    }
    std::unique_ptr<edit::ClipInfo> info = clipInfoAt(track, index);
    if (!info || !info->cut || !bypassableKeys().contains(key))
        return false;
    bool found = false;
    for (int i = 0; i < info->cut->filter_count(); ++i) {
        std::unique_ptr<edit::Filter> f(info->cut->filter(i));
        if (!f || !f->is_valid() || bypassKeyOf(*f) != key)
            continue;
        f->set("disable", bypass ? 1 : 0);
        found = true;
    }
    return found;
}

void Timeline::applyEditExtras(int track, int index, const ClipEntry &e) {
    // V1 now supports exactly the same transform controls as upper video tracks.
    if (track == 0 &&
        (e.rectX != 0 || e.rectY != 0 || e.rectW != 1 || e.rectH != 1 || e.rotation != 0 || e.anchorX != .5 || e.anchorY != .5))
        setRectAt(track, index, e.rectX, e.rectY, e.rectW, e.rectH, e.rotation, e.anchorX, e.anchorY);
    if (track == 0) {
        if (e.opacity != 1.)
            setOpacityAt(track, index, e.opacity);
        if (!e.opacityKf.isEmpty())
            setOpacityKfAt(track, index, e.opacityKf);
    }
    auto info = clipInfoAt(track, index);
    if (!info || !info->cut)
        return;
    const QString nestId = ClipEntry::orDefault(e.nestId);
    if (info->producer && ProjectFile::isProject(e.path) && !nestId.isEmpty())
        info->producer->set("omavid:nestId", nestId.toUtf8().constData());
    // The edit engine reads its own keys from the map as it came.
    EditEngine::attach(*m_profile, *info->cut, e.raw, info->frame_in, info->frame_out);
    reorderAttributeFilters(info->cut);
}

bool Timeline::applyMotionPresetAt(int track, int index, const QString &preset,
                                     const QVariantMap &options, QString *error) {
    if (error) error->clear();
    const auto entry = entryMapOn(track, index);
    if (trackLocked(track) || entry.isEmpty() || entry.value(ClipKey::audioOnly).toBool()
        || entry.value(ClipKey::gap).toBool() || entry.value(ClipKey::adjust).toBool()) {
        if (error) *error = QCoreApplication::translate("Timeline", "Select a picture or title clip on an unlocked track for motion presets.");
        return false;
    }
    const auto patch = EditEngine::motionPreset(entry, preset, options, fps(), error);
    if (patch.isEmpty()) return false;
    return setEditProperties(track, index, patch);
}

bool Timeline::setEditProperties(int track, int index, const QVariantMap &patch) {
    if (trackLocked(track) || patch.isEmpty())
        return false;
    // Keys for several effects' controls at once, {uid: {control: keys}} (and
    // their masks' in "videoEffectMaskKeys", {uid: {field: keys}}),
    // with whatever else the patch changes: one step (Keyframes > Paste, Cut).
    if (patch.contains("videoEffectKeys") || patch.contains("videoEffectMaskKeys")) {
        auto rest = patch;
        const auto byUid = rest.take("videoEffectKeys").toMap(), maskByUid = rest.take("videoEffectMaskKeys").toMap();
        auto stack = entryMapOn(track, index).value(ClipKey::videoEffects).toList();
        for (QVariant &v : stack) {
            auto state = v.toMap();
            const QString uid = state.value("uid").toString();
            // The controls' keys, then the mask's: `own` is "keys" or "maskKeys".
            const auto merged = [&](const QVariantMap &given, const char *own) {
                auto keys = state.value(QLatin1String(own)).toMap();
                for (auto it = given.begin(); it != given.end(); ++it) {
                    if (it.value().toList().isEmpty()) keys.remove(it.key());
                    else keys[it.key()] = it.value();
                }
                if (keys.isEmpty()) state.remove(QLatin1String(own)); else state[QLatin1String(own)] = keys;
                return keys;
            };
            if (byUid.contains(uid) && !VideoEffects::validateKeys(state.value("id").toString(), merged(byUid.value(uid).toMap(), "keys")).isEmpty())
                return false;
            if (maskByUid.contains(uid) && !VideoEffects::validateMaskKeys(state.value("id").toString(), state.value("mask").toMap(),
                                                                         merged(maskByUid.value(uid).toMap(), "maskKeys")).isEmpty())
                return false;
            v = state;
        }
        const bool changed = setVideoEffectsAt(track, index, stack);
        if (rest.isEmpty()) return changed;
        return setEditProperties(track, index, rest) || changed;
    }
    // The shared pointer-edit queue coalesces the most recent effect values
    // and flushes them before save, undo, or a different editing command.
    if (patch.contains("videoEffectUpdate")) {
        const auto update = patch.value("videoEffectUpdate").toMap();
        if (patch.size() != 1 || update.size() != 2 || !update.contains("uid")
            || update.value("values").metaType().id() != QMetaType::QVariantMap) return false;
        return editVideoEffectAt(track, index, "update", update.value("uid").toString(), update.value("values").toMap());
    }
    if (patch.contains("resetEffect")) {
        const auto key = patch.value("resetEffect").toString();
        auto entry = entryMapOn(track, index);
        if (entry.isEmpty())
            return false;
        if (key == "rect" || key == "color" || key == "crop") {
            auto keys = entry.value(ClipKey::editKeys).toMap();
            QVariantMap clear;
            for (const auto &param : EditEngine::parameters(key)) {
                keys.remove(param);
                clear[param] =
                    (param == "rectW" || param == "rectH" || param == "colorC" || param == "colorS") ? 1.
                    : (param == "anchorX" || param == "anchorY")                                     ? .5
                                                                                                     : 0.;
            }
            if (key == "crop")
                clear[ClipKey::cropInvert] = false;
            clear[ClipKey::editKeys] = keys;
            return setEditProperties(track, index, clear);
        }
        if (key == "stab")
            return setStabAt(track, index, QString(), 4, 10);
        if (key == "grade")
            return setGradeAt(track, index, {});
        if (key == "audioMap")
            return setAudioMapAt(track, index, {});
        if (key == "normalize")
            return setNormalizeAt(track, index, false);
        if (attributeGroups().contains(key))
            return pasteAttributes(track, index, {}, QStringList{key});
        return false;
    }
    auto clips = entries();
    const auto before = clips;
    for (int i = 0; i < clips.size(); ++i) {
        auto e = clips[i].toMap();
        if (e.value(ClipKey::lane).toInt() != track || e.value(ClipKey::index).toInt() != index)
            continue;
        if (e.value(ClipKey::gap).toBool())
            return false;
        if (e.value(ClipKey::audioOnly).toBool() && (patch.size() != 1 || !patch.contains(ClipKey::effectOrder)))
            return false;
        const QStringList groups = {"rect", "crop", "color"};
        QStringList allowed = {"editKeys", "effectOrder", "opacity", "opacityKf"};
        for (const auto &g : groups)
            allowed += EditEngine::parameters(g);
        allowed << "cropInvert";
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (!allowed.contains(it.key()))
                return false;
            e[it.key()] = it.value();
        }
        if (e.contains(ClipKey::opacity) && (!std::isfinite(e.value(ClipKey::opacity).toDouble()) ||
                                      e.value(ClipKey::opacity).toDouble() < 0 || e.value(ClipKey::opacity).toDouble() > 1))
            return false;
        if (patch.contains(ClipKey::opacityKf)) {
            QVariantList opacity;
            if (patch.value(ClipKey::opacityKf).metaType().id() != QMetaType::QVariantList
                || !EditEngine::normalizeOpacityKeys(patch.value(ClipKey::opacityKf).toList(), &opacity)) return false;
            e[ClipKey::opacityKf] = opacity;
        }
        const auto keys = e.value(ClipKey::editKeys).toMap();
        QVariantMap normal;
        auto valid = [](const QString &key, double v) {
            if (!std::isfinite(v))
                return false;
            if (key == "rectW" || key == "rectH")
                return v >= .001 && v <= 20;
            if (key == "anchorX" || key == "anchorY" || key == "cropFeather" || key == "opacity")
                return v >= 0 && v <= 1;
            if (key.startsWith("crop"))
                return v >= 0 && v <= .95;
            if (key == "colorB")
                return v >= -1 && v <= 1;
            if (key == "colorC")
                return v >= 0 && v <= 2;
            if (key == "colorS")
                return v >= 0 && v <= 3;
            return std::abs(v) <= 10000;
        };
        for (const auto &g : groups)
            for (const auto &key : EditEngine::parameters(g))
                if (e.contains(key) && !valid(key, e.value(key).toDouble()))
                    return false;
        if (e.value(ClipKey::cropL).toDouble() + e.value(ClipKey::cropR).toDouble() > .95 ||
            e.value(ClipKey::cropT).toDouble() + e.value(ClipKey::cropB).toDouble() > .95)
            return false;
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!allowed.contains(it.key()) || it.key() == "editKeys" || it.key() == "effectOrder" ||
                it.key() == "cropInvert" || it.key() == "opacity" || it.key() == "opacityKf")
                return false;
            QMap<int, QVariantMap> unique;
            for (const auto &v : it.value().toList()) {
                auto k = v.toMap();
                const int frame = k.value("frame").toInt();
                if (!k.contains("value") || !valid(it.key(), k.value("value").toDouble()) ||
                    qAbs(qint64(frame)) > 100000000)
                    return false;
                const auto mode = k.value("interpolation", "linear").toString();
                if (mode != "linear" && mode != "hold" && mode != "smooth")
                    return false;
                unique[frame] = QVariantMap{
                    {"frame", frame}, {"value", k.value("value").toDouble()}, {"interpolation", mode}};
            }
            QVariantList list;
            for (const auto &k : unique)
                list << k;
            if (!list.isEmpty())
                normal[it.key()] = list;
        }
        e[ClipKey::editKeys] = normal;
        QStringList order;
        for (const auto &key : e.value(ClipKey::effectOrder).toStringList()) {
            if (!bypassableKeys().contains(key) || order.contains(key))
                return false;
            order << key;
        }
        e[ClipKey::effectOrder] = order;
        if (e == clips[i].toMap())
            return false;
        // Picture controls change cut filters, not media or timing. Keep the
        // open producers and decoded frames while dragging any of these values.
        const auto old = clips[i].toMap();
        const auto oldKeys = old.value(ClipKey::editKeys).toMap();
        auto changedGroup = [&](const QString &group) {
            for (const auto &key : EditEngine::parameters(group))
                if (patch.contains(key) || oldKeys.value(key) != normal.value(key)) return true;
            return group == "crop" && patch.contains(ClipKey::cropInvert);
        };
        bool ok = true;
        if (changedGroup("rect"))
            ok = setRectAt(track, index, e.value(ClipKey::rectX).toDouble(), e.value(ClipKey::rectY).toDouble(),
                           e.value(ClipKey::rectW, 1.).toDouble(), e.value(ClipKey::rectH, 1.).toDouble(),
                           e.value(ClipKey::rotation).toDouble(), e.value(ClipKey::anchorX, .5).toDouble(),
                           e.value(ClipKey::anchorY, .5).toDouble()) && ok;
        if (changedGroup("crop"))
            ok = setCropAt(track, index, e.value(ClipKey::cropL).toDouble(), e.value(ClipKey::cropR).toDouble(),
                           e.value(ClipKey::cropT).toDouble(), e.value(ClipKey::cropB).toDouble(),
                           e.value(ClipKey::cropFeather).toDouble(), e.value(ClipKey::cropInvert).toBool()) && ok;
        if (changedGroup("color"))
            ok = setColorAt(track, index, e.value(ClipKey::colorB).toDouble(),
                            e.value(ClipKey::colorC, 1.).toDouble(), e.value(ClipKey::colorS, 1.).toDouble()) && ok;
        if (patch.contains(ClipKey::opacity) || patch.contains(ClipKey::opacityKf)) {
            ok = setOpacityAt(track, index, e.value(ClipKey::opacity, 1.).toDouble()) && ok;
            ok = setOpacityKfAt(track, index, e.value(ClipKey::opacityKf).toList()) && ok;
        }
        clips[i] = e;
        if (!ok) return rebuild(clips, before); // restore atomically if a filter is unavailable
        auto info = clipInfoAt(track, index);
        if (!info || !info->cut) return rebuild(clips, before);
        if (patch.contains(ClipKey::effectOrder))
            for (int f = info->cut->filter_count() - 1; f >= 0; --f) {
                std::unique_ptr<edit::Filter> filter(info->cut->filter(f));
                if (filter && filter->get("omavid:effectOrder")) info->cut->detach(*filter);
            }
        EditEngine::attach(*m_profile, *info->cut, e, info->frame_in, info->frame_out);
        reorderAttributeFilters(info->cut);
        for (const auto &key : e.value(ClipKey::bypass).toStringList()) setBypassAt(track, index, key, true);
        return true;
    }
    return false;
}
