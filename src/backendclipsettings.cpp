#include "backend_p.h"

#include <cmath>

static QString blendName(int mode) {
    switch (mode) {
    case 12: return QCoreApplication::translate("Backend", "Add");
    case 13: return QCoreApplication::translate("Backend", "Multiply");
    case 14: return QCoreApplication::translate("Backend", "Screen");
    case 15: return QCoreApplication::translate("Backend", "Overlay");
    case 16: return QCoreApplication::translate("Backend", "Darken");
    case 17: return QCoreApplication::translate("Backend", "Lighten");
    case 18: return QCoreApplication::translate("Backend", "Colour Dodge");
    case 19: return QCoreApplication::translate("Backend", "Colour Burn");
    case 20: return QCoreApplication::translate("Backend", "Hard Light");
    case 21: return QCoreApplication::translate("Backend", "Soft Light");
    case 22: return QCoreApplication::translate("Backend", "Difference");
    case 23: return QCoreApplication::translate("Backend", "Exclusion");
    default: return QCoreApplication::translate("Backend", "Normal");
    }
}

namespace {

QString panStatus(double pan) {
    if (pan == 0.0)
        return QCoreApplication::translate("Backend", "Pan centred");
    const int amount = qRound(qAbs(pan) * 100);
    return pan < 0 ? QCoreApplication::translate("Backend", "Pan L%1%").arg(amount)
                   : QCoreApplication::translate("Backend", "Pan R%1%").arg(amount);
}

QString eqStatus(const QString &preset) {
    if (preset.isEmpty())
        return QCoreApplication::translate("Backend", "EQ: Flat");
    if (preset == QStringLiteral("voice"))
        return QCoreApplication::translate("Backend", "EQ: Voice Enhance");
    if (preset == QStringLiteral("hum"))
        return QCoreApplication::translate("Backend", "EQ: Hum Reduction");
    QString label = preset;
    label.replace(QLatin1Char('-'), QLatin1Char(' '));
    return QCoreApplication::translate("Backend", "EQ: %1").arg(label);
}
}

static bool sameCrop(const QVariantMap &e, double l, double r, double t, double b,
                     double feather, bool invert) {
    return e.value(ClipKey::cropL).toDouble() == l
           && e.value(ClipKey::cropR).toDouble() == r
           && e.value(ClipKey::cropT).toDouble() == t
           && e.value(ClipKey::cropB).toDouble() == b
           && e.value(ClipKey::cropFeather).toDouble() == feather
           && e.value(ClipKey::cropInvert).toBool() == invert;
}

static QString cropStatus(double l, double r, double t, double b, double feather, bool invert) {
    if (l == 0.0 && r == 0.0 && t == 0.0 && b == 0.0)
        return QCoreApplication::translate("Backend", "Crop removed");
    QString s = invert ? QCoreApplication::translate("Backend", "Cropped (inverted)")
                       : QCoreApplication::translate("Backend", "Cropped");
    if (feather > 0.0)
        s += QCoreApplication::translate("Backend", " · feather %1%").arg(qRound(feather * 100.0));
    return s;
}

namespace {

// One property's keys: [{frame, ...}] by frame.
QVariantList keysByFrame(QVariantList keys) {
    std::sort(keys.begin(), keys.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("frame")).toInt() < b.toMap().value(QStringLiteral("frame")).toInt();
    });
    return keys;
}
}

void Backend::setDissolveAlign(int track, int index, const QString &align) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(align == QStringLiteral("start") ? tr("Dissolve starts at the cut")
              : align == QStringLiteral("end") ? tr("Dissolve ends at the cut")
              : align == QStringLiteral("centre") ? tr("Dissolve centred on the cut")
              : tr("Dissolve overlaps the clips"));
    emit setMixAlignRequested(track, index, align);
}

namespace {

// The clip at (track, index) and the one after it on that track, from the
// window's two lists: the pair a transition sits on.
bool transitionPair(const QVariantList &story, const QVariantList &connected, int track, int index,
                    QVariantMap *a, QVariantMap *b) {
    for (const QVariant &v : track == 0 ? story : connected) {
        const QVariantMap e = v.toMap();
        if (e.value(ClipKey::track).toInt() != track)
            continue;
        if (e.value(ClipKey::index).toInt() == index)
            *a = e;
        else if (e.value(ClipKey::index).toInt() == index + 1)
            *b = e;
    }
    return !a->isEmpty() && !b->isEmpty();
}
}

int Backend::transitionSideLimit(int track, int index, bool after) const {
    QVariantMap a, b;
    return transitionPair(m_timeline, m_connected, track, index, &a, &b) ? Timeline::mixSideLimit(a, b, after) : 0;
}

void Backend::setTransitionSides(int track, int index, int before, int after) {
    QVariantMap a, b;
    if (m_exporting || !transitionPair(m_timeline, m_connected, track, index, &a, &b))
        return;
    const Timeline::MixParts parts = Timeline::mixParts(a);
    if (parts.extA + parts.extB + parts.overlap < 1)
        return;
    before = qBound(0, before, Timeline::mixSideLimit(a, b, false));
    after = qBound(0, after, Timeline::mixSideLimit(a, b, true));
    if (before == parts.extB + parts.overlap && after == parts.extA)
        return;
    setStatus(before + after < 1 ? tr("Transition removed")
                                 : tr("Transition: %1 before the cut, %2 after")
                                       .arg(Interchange::durationText(before, fps()), Interchange::durationText(after, fps())));
    emit setTransitionSidesRequested(track, index, before, after);
}

void Backend::setBlend(int track, int index, int mode) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Blend mode: %1").arg(blendName(mode)));
    emit setBlendRequested(track, index, mode);
}

void Backend::setClipScaling(int track, int index, const QString &scaling) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(scaling == QStringLiteral("fill") ? tr("Scaling: fill the frame")
              : scaling == QStringLiteral("stretch") ? tr("Scaling: stretch to the frame")
              : scaling == QStringLiteral("original") ? tr("Scaling: original size")
              : scaling == QStringLiteral("fit") ? tr("Scaling: fit the whole picture")
              : tr("Scaling: as the timeline"));
    emit setScalingRequested(track, index, scaling);
}

QVariantList Backend::videoEffectCatalogue() const { return VideoEffects::catalogue(); }
QVariantList Backend::transitionStyles() const { return render::transitionChoices(); }

bool Backend::editVideoEffect(int track, int index, const QString &action,
                              const QString &uid, const QVariantMap &values) {
    if (m_exporting || track < 0 || index < 0) {
        setStatus(tr("Select an unlocked picture clip while no export is running."));
        return false;
    }
    viewTimeline();
    const auto patch = plainVariant(values).toMap();
    if (action == QLatin1String("findBackdrop") || action == QLatin1String("pickBackdrop")) {
        const bool find = action == QLatin1String("findBackdrop");
        emit findKeyBackdropRequested(track, index, uid, patch.value(QStringLiteral("clipFrame")).toInt(),
                                      find ? -1. : patch.value(QStringLiteral("x")).toDouble(), find ? -1. : patch.value(QStringLiteral("y")).toDouble());
        return true;
    }
    if (m_editGesture && action == "update")
        m_worker->queueEditProperties(track, index,
            {{"videoEffectUpdate", QVariantMap{{"uid", uid}, {"values", patch}}}}, m_editGesture);
    else
        emit videoEffectRequested(track, index, action, uid, patch);
    return true;
}

void Backend::setLens(int track, int index, double k1, double k2, double cx, double cy) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(qFuzzyIsNull(k1) && qFuzzyIsNull(k2) ? tr("Lens correction removed")
                                                     : tr("Lens correction k1 %1 k2 %2")
                                                           .arg(k1, 0, 'f', 2).arg(k2, 0, 'f', 2));
    emit setLensRequested(track, index, k1, k2, cx, cy);
}

void Backend::stabilize(int track, int index, int shakiness, int smoothing) {
    if (m_exporting || track < 0 || index < 0)
        return;
    if(entryOn(track,index).contains(ClipKey::retime)) {
        setStatus(tr("Render this clip in place before analysing stabilisation with speed segments."));return;
    }
    if (!m_stabJob.isEmpty()) {
        setStatus(tr("Stabiliser busy — one analysis at a time"));
        return;
    }
    setStatus(tr("Analysing motion…"));
    emit stabilizeRequested(track, index, shakiness, smoothing);
}

void Backend::removeStab(int track, int index) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Stabilisation removed"));
    emit removeStabRequested(track, index);
}

void Backend::onStabProgress(int track, int index, int percent) {
    if (percent >= 100 || percent < 0) {
        m_stabJob = QVariantMap();
        setStatus(percent < 0 ? tr("Stabilisation failed — no analysis for that clip")
                              : tr("Stabilised"));
    } else {
        m_stabJob = QVariantMap{{QStringLiteral("track"), track},
                                {QStringLiteral("index"), index},
                                {QStringLiteral("percent"), percent}};
    }
    emit stabJobChanged();
}

void Backend::onStabApplied(int, int, double zoom, double steadied) {
    // Said plainly when the shake is more than the most zoom can fill: the
    // clip is steadied less rather than show an edge.
    if (steadied < 1)
        setStatus(tr("Stabilised · zoom %1% · the shake is too strong to steady fully without edges at the most zoom, so it is steadied %2% of the way")
                      .arg(zoom, 0, 'f', 1).arg(std::floor(steadied * 100)));
    else
        setStatus(tr("Stabilised · zoom %1%").arg(zoom, 0, 'f', 1));
}

void Backend::setClipVolumeKf(int timelineIndex, const QVariantList &kf) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    setStatus(kf.isEmpty()
                  ? tr("Volume keyframes cleared")
                  : tr("Volume keyframes: %1").arg(kf.size()));
    emit setVolumeKfRequested(timelineIndex, kf);
}

void Backend::setConnectedVolumeKf(int connectedIndex,
                                   const QVariantList &kf) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    setStatus(kf.isEmpty()
                  ? tr("Volume keyframes cleared")
                  : tr("Volume keyframes: %1").arg(kf.size()));
    emit setConnectedVolumeKfRequested(connectedIndex, kf);
}

void Backend::setConnectedOpacity(int connectedIndex, double alpha) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    setStatus(alpha >= 1.0
                  ? tr("Opacity reset")
                  : tr("Opacity %1%").arg(qRound(alpha * 100)));
    emit setOpacityRequested(connectedIndex, alpha);
}

void Backend::setConnectedOpacityKf(int connectedIndex,
                                    const QVariantList &kf) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    setStatus(kf.isEmpty()
                  ? tr("Opacity keyframes cleared")
                  : tr("Opacity keyframes: %1").arg(kf.size()));
    emit setOpacityKfRequested(connectedIndex, kf);
}

void Backend::adjustVolume(int timelineIndex, double deltaDb) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const double current = m_timeline.at(timelineIndex).toMap()
                               .value(ClipKey::volume).toDouble();
    setClipVolume(timelineIndex, current + deltaDb);
}

void Backend::setClipVolume(int timelineIndex, double db) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    db = qBound(-60.0, db, 12.0);
    const double current = m_timeline.at(timelineIndex).toMap()
                               .value(ClipKey::volume).toDouble();
    if (db == current)
        return;
    if (db <= -60.0)
        setStatus(tr("Clip silenced"));
    else if (db == 0.0)
        setStatus(tr("Volume reset to 0 dB"));
    else
        setStatus(tr("Volume %1%2 dB")
                      .arg(db > 0 ? QStringLiteral("+") : QString())
                      .arg(db, 0, 'f', 0));
    emit setVolumeRequested(timelineIndex, db);
}

void Backend::adjustConnectedVolume(int connectedIndex, double deltaDb) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const double current = m_connected.at(connectedIndex).toMap()
                               .value(ClipKey::volume).toDouble();
    setConnectedVolume(connectedIndex, current + deltaDb);
}

void Backend::setConnectedVolume(int connectedIndex, double db) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    db = qBound(-60.0, db, 12.0);
    const double current = m_connected.at(connectedIndex).toMap()
                               .value(ClipKey::volume).toDouble();
    if (db == current)
        return;
    if (db <= -60.0)
        setStatus(tr("Clip silenced"));
    else if (db == 0.0)
        setStatus(tr("Volume reset to 0 dB"));
    else
        setStatus(tr("Volume %1%2 dB")
                      .arg(db > 0 ? QStringLiteral("+") : QString())
                      .arg(db, 0, 'f', 0));
    emit setConnectedVolumeRequested(connectedIndex, db);
}

void Backend::setClipPan(int timelineIndex, double pan) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    pan = qBound(-1.0, pan, 1.0);
    if (m_timeline.at(timelineIndex).toMap()
            .value(ClipKey::pan).toDouble() == pan)
        return;
    setStatus(panStatus(pan));
    emit setPanRequested(timelineIndex, pan);
}

void Backend::setConnectedPan(int connectedIndex, double pan) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    pan = qBound(-1.0, pan, 1.0);
    if (m_connected.at(connectedIndex).toMap()
            .value(ClipKey::pan).toDouble() == pan)
        return;
    setStatus(panStatus(pan));
    emit setConnectedPanRequested(connectedIndex, pan);
}

void Backend::setClipEq(int timelineIndex, const QString &preset) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    if (m_timeline.at(timelineIndex).toMap()
            .value(ClipKey::eq).toString() == preset)
        return;
    setStatus(eqStatus(preset));
    emit setEqRequested(timelineIndex, preset);
}

void Backend::setConnectedEq(int connectedIndex, const QString &preset) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    if (m_connected.at(connectedIndex).toMap()
            .value(ClipKey::eq).toString() == preset)
        return;
    setStatus(eqStatus(preset));
    emit setConnectedEqRequested(connectedIndex, preset);
}

void Backend::detachAudio(int timelineIndex) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    if (entry.value(ClipKey::gap).toBool()
        || entry.value(ClipKey::videoOnly).toBool()) {
        setStatus(tr("No audio to detach"));
        return;
    }
    if (entry.value(ClipKey::mixIn).toInt() > 0
        || entry.value(ClipKey::mixOut).toInt() > 0) {
        setStatus(tr("Remove the dissolve before detaching audio"));
        return;
    }
    setStatus(tr("Audio detached from %1")
                  .arg(entry.value(ClipKey::name).toString()));
    emit detachAudioRequested(timelineIndex);
}

void Backend::setClipCrop(int timelineIndex, double l, double r, double t, double b,
                          double feather, bool invert) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    if (sameCrop(m_timeline.at(timelineIndex).toMap(), l, r, t, b, feather, invert))
        return;
    setStatus(cropStatus(l, r, t, b, feather, invert));
    emit setCropRequested(timelineIndex, l, r, t, b, feather, invert);
}

void Backend::setConnectedCrop(int connectedIndex, double l, double r, double t, double b,
                               double feather, bool invert) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    if (sameCrop(m_connected.at(connectedIndex).toMap(), l, r, t, b, feather, invert))
        return;
    setStatus(cropStatus(l, r, t, b, feather, invert));
    emit setConnectedCropRequested(connectedIndex, l, r, t, b, feather, invert);
}

void Backend::setConnectedAnchor(int connectedIndex, double anchorX, double anchorY) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (e.value(ClipKey::anchorX, 0.5).toDouble() == anchorX
        && e.value(ClipKey::anchorY, 0.5).toDouble() == anchorY)
        return;
    setStatus(tr("Anchor %1%, %2%").arg(qRound(anchorX * 100.0)).arg(qRound(anchorY * 100.0)));
    emit setConnectedAnchorRequested(connectedIndex, anchorX, anchorY);
}

void Backend::setEditProperties(int track, int index, const QVariantMap &patch) {
    if (m_exporting || track < 0 || index < 0) return;
    viewTimeline(); // inspector changes affect the edit, not the source preview
    const auto values = plainVariant(patch).toMap();
    if (m_editGesture)
        m_worker->queueEditProperties(track, index, values, m_editGesture);
    else
        emit editPropertiesRequested(track, index, values);
}

bool Backend::copyKeyframes(int track, int index) {
    const QVariantMap entry = entryOn(track, index);
    if (entry.isEmpty()) return false;
    QVariantMap editKeys;
    const QVariantMap source = entry.value(ClipKey::editKeys).toMap();
    for (auto it = source.begin(); it != source.end(); ++it)
        if (!it.value().toList().isEmpty()) editKeys.insert(it.key(), keysByFrame(it.value().toList()));
    const QVariantList opacity = keysByFrame(entry.value(ClipKey::opacityKf).toList());
    int first = std::numeric_limits<int>::max(), count = opacity.size();
    for (const QVariant &key : opacity) first = qMin(first, key.toMap().value(QStringLiteral("frame")).toInt());
    for (const QVariant &keys : std::as_const(editKeys)) {
        count += keys.toList().size();
        first = qMin(first, keys.toList().first().toMap().value(QStringLiteral("frame")).toInt());
    }
    // The effects' keys too, each known by what it is and which of its kind
    // (the second blur's keys go to the second blur of the clip pasted on).
    QVariantList effectKeys;
    QHash<QString, int> seen;
    for (const QVariant &v : entry.value(ClipKey::videoEffects).toList()) {
        const QVariantMap state = v.toMap();
        const QString id = state.value(QStringLiteral("id")).toString();
        const int nth = seen[id]++;
        QVariantMap curves;
        const QVariantMap own = state.value(QStringLiteral("keys")).toMap();
        for (auto it = own.begin(); it != own.end(); ++it) {
            const QVariantList keys = keysByFrame(it.value().toList());
            if (keys.isEmpty()) continue;
            curves.insert(it.key(), keys);
            count += keys.size();
            first = qMin(first, keys.first().toMap().value(QStringLiteral("frame")).toInt());
        }
        // Its mask's keys with them, and the shape they were made for.
        QVariantMap maskCurves;
        const QVariantMap ownMask = state.value(QStringLiteral("maskKeys")).toMap();
        for (auto it = ownMask.begin(); it != ownMask.end(); ++it) {
            const QVariantList keys = keysByFrame(it.value().toList());
            if (keys.isEmpty()) continue;
            maskCurves.insert(it.key(), keys);
            count += keys.size();
            first = qMin(first, keys.first().toMap().value(QStringLiteral("frame")).toInt());
        }
        if (!curves.isEmpty() || !maskCurves.isEmpty())
            effectKeys.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("nth"), nth}, {QStringLiteral("keys"), curves},
                                          {QStringLiteral("maskKeys"), maskCurves},
                                          {QStringLiteral("maskShape"), state.value(QStringLiteral("mask")).toMap().value(QStringLiteral("shape"))}});
    }
    const QString name = entry.value(ClipKey::name).toString();
    if (!count) {
        setStatus(tr("%1 has no keyframes to copy").arg(name));
        return false;
    }
    // Kept with the first key at 0, so paste places them by the playhead.
    const auto shifted = [first](const QVariantList &keys) {
        QVariantList out;
        for (const QVariant &v : keys) {
            QVariantMap key = v.toMap();
            key.insert(QStringLiteral("frame"), key.value(QStringLiteral("frame")).toInt() - first);
            out.append(key);
        }
        return out;
    };
    for (auto it = editKeys.begin(); it != editKeys.end(); ++it) it.value() = shifted(it.value().toList());
    for (QVariant &v : effectKeys) {
        QVariantMap effect = v.toMap();
        for (const QString &kind : {QStringLiteral("keys"), QStringLiteral("maskKeys")}) {
            QVariantMap curves = effect.value(kind).toMap();
            for (auto it = curves.begin(); it != curves.end(); ++it) it.value() = shifted(it.value().toList());
            effect.insert(kind, curves);
        }
        v = effect;
    }
    m_keyframeClipboard = {{QStringLiteral("editKeys"), editKeys}, {QStringLiteral("opacityKf"), shifted(opacity)},
                           {QStringLiteral("effectKeys"), effectKeys}};
    emit keyframeClipboardChanged();
    setStatus(tr("Copied %n keyframe(s) from %1", nullptr, count).arg(name));
    return true;
}

bool Backend::copyTrackAsMotion(int track, int index, const QVariantList &points) {
    const QVariantMap entry = entryOn(track, index);
    if (entry.isEmpty()) return false;
    struct Point { int frame; double x, y; };
    QMap<int, Point> byFrame;
    for (const QVariant &v : plainVariant(points).toList()) {
        const QVariantMap p = v.toMap();
        if (!p.contains(QStringLiteral("x")) || !p.contains(QStringLiteral("y"))) continue;
        const Point point{p.value(QStringLiteral("frame")).toInt(), p.value(QStringLiteral("x")).toDouble(), p.value(QStringLiteral("y")).toDouble()};
        if (std::isfinite(point.x) && std::isfinite(point.y)) byFrame.insert(point.frame, point);
    }
    const QList<Point> path = byFrame.values();
    if (path.size() < 2) {
        setStatus(tr("Track the window first: there is no path to copy"));
        return false;
    }
    // A key only where the path bends: a point within a five-hundredth of the
    // frame of the straight line between its kept neighbours is left out.
    QList<bool> kept(path.size(), false);
    kept.first() = kept.last() = true;
    QList<QPair<int, int>> spans{{0, int(path.size()) - 1}};
    while (!spans.isEmpty()) {
        const auto [a, b] = spans.takeLast();
        double worst = 0;
        int at = -1;
        for (int i = a + 1; i < b; ++i) {
            const double u = double(path[i].frame - path[a].frame) / (path[b].frame - path[a].frame);
            const double off = std::max(std::abs(path[i].x - (path[a].x + (path[b].x - path[a].x) * u)),
                                        std::abs(path[i].y - (path[a].y + (path[b].y - path[a].y) * u)));
            if (off > worst) { worst = off; at = i; }
        }
        if (at >= 0 && worst > 0.002) { kept[at] = true; spans.append({a, at}); spans.append({at, b}); }
    }
    QVariantList across, down;
    for (int i = 0; i < path.size(); ++i) {
        if (!kept[i]) continue;
        const int frame = path[i].frame - path.first().frame;
        across.append(QVariantMap{{QStringLiteral("frame"), frame}, {QStringLiteral("value"), path[i].x - path.first().x}, {QStringLiteral("interpolation"), QStringLiteral("linear")}});
        down.append(QVariantMap{{QStringLiteral("frame"), frame}, {QStringLiteral("value"), path[i].y - path.first().y}, {QStringLiteral("interpolation"), QStringLiteral("linear")}});
    }
    m_keyframeClipboard = {{QStringLiteral("editKeys"), QVariantMap{{QStringLiteral("rectX"), across}, {QStringLiteral("rectY"), down}}},
                           {QStringLiteral("opacityKf"), QVariantList()}, {QStringLiteral("effectKeys"), QVariantList()},
                           {QStringLiteral("motion"), true},
                           {QStringLiteral("timelineFrame"), entry.value(ClipKey::start).toInt() + path.first().frame}};
    emit keyframeClipboardChanged();
    setStatus(tr("Copied the track as %n motion keyframe(s): select the clip that should follow and choose Keyframes ▸ Paste", nullptr, int(across.size())));
    return true;
}

bool Backend::cutKeyframes(int track, int index) {
    if (m_exporting || !copyKeyframes(track, index)) return false;
    const QVariantMap entry = entryOn(track, index);
    QVariantMap patch{{QStringLiteral("editKeys"), QVariantMap()}, {QStringLiteral("opacityKf"), QVariantList()}};
    QVariantMap effectKeys, maskKeys;
    for (const QVariant &v : entry.value(ClipKey::videoEffects).toList()) {
        QVariantMap none, noMask;
        const QVariantMap own = v.toMap().value(QStringLiteral("keys")).toMap(), ownMask = v.toMap().value(QStringLiteral("maskKeys")).toMap();
        for (auto it = own.begin(); it != own.end(); ++it) none.insert(it.key(), QVariantList());
        for (auto it = ownMask.begin(); it != ownMask.end(); ++it) noMask.insert(it.key(), QVariantList());
        if (!none.isEmpty()) effectKeys.insert(v.toMap().value(QStringLiteral("uid")).toString(), none);
        if (!noMask.isEmpty()) maskKeys.insert(v.toMap().value(QStringLiteral("uid")).toString(), noMask);
    }
    if (!effectKeys.isEmpty()) patch.insert(QStringLiteral("videoEffectKeys"), effectKeys);
    if (!maskKeys.isEmpty()) patch.insert(QStringLiteral("videoEffectMaskKeys"), maskKeys);
    setEditProperties(track, index, patch);
    setStatus(tr("Cut the keyframes from %1").arg(entry.value(ClipKey::name).toString()));
    return true;
}

bool Backend::pasteKeyframes(int track, int index, int clipFrame) {
    const QVariantMap entry = entryOn(track, index);
    if (m_exporting || entry.isEmpty() || m_keyframeClipboard.isEmpty()) return false;
    const int frames = entry.value(ClipKey::frames).toInt();
    const QString name = entry.value(ClipKey::name).toString();
    // A copied track: the offsets it moved by, added to where this clip
    // stands, at the timeline's own moments (the part of it this clip covers).
    QVariantMap copiedKeys = m_keyframeClipboard.value(QStringLiteral("editKeys")).toMap();
    const bool motion = m_keyframeClipboard.value(QStringLiteral("motion")).toBool();
    if (motion) {
        const int offset = m_keyframeClipboard.value(QStringLiteral("timelineFrame")).toInt() - entry.value(ClipKey::start).toInt();
        for (auto it = copiedKeys.begin(); it != copiedKeys.end(); ++it) {
            const QVariantList curve = it.value().toList();
            const double base = entry.value(it.key()).toDouble();
            const int first = offset + curve.first().toMap().value(QStringLiteral("frame")).toInt();
            const int last = offset + curve.last().toMap().value(QStringLiteral("frame")).toInt();
            QMap<int, QVariant> placed;
            const auto put = [&](int frame) {
                placed.insert(frame, QVariantMap{{QStringLiteral("frame"), frame}, {QStringLiteral("value"), base + EditEngine::valueAt(curve, frame - offset, 0.)},
                                                 {QStringLiteral("interpolation"), QStringLiteral("linear")}});
            };
            if (last >= 0 && first < frames) {
                put(std::max(first, 0));
                put(std::min(last, frames - 1));
                for (const QVariant &v : curve)
                    if (const int frame = offset + v.toMap().value(QStringLiteral("frame")).toInt(); frame >= 0 && frame < frames) put(frame);
            }
            it.value() = QVariantList(placed.values());
        }
        if (copiedKeys.value(QStringLiteral("rectX")).toList().isEmpty()) {
            setStatus(tr("%1 does not play while the tracked clip does, so it has nothing to follow").arg(name));
            return false;
        }
        clipFrame = 0;
    }
    if (clipFrame < 0 || clipFrame >= frames) {
        setStatus(tr("Put the playhead over %1 to paste keyframes onto it").arg(name));
        return false;
    }
    int pasted = 0;
    // The pasted keys replace a property's own keys over the stretch they
    // cover; its keys before and after it stay.
    const auto merge = [&](const QVariantList &own, const QVariantList &copied) {
        if (copied.isEmpty()) return own;
        const int from = clipFrame + copied.first().toMap().value(QStringLiteral("frame")).toInt();
        const int to = clipFrame + copied.last().toMap().value(QStringLiteral("frame")).toInt();
        QVariantList out;
        for (const QVariant &v : own) {
            const int frame = v.toMap().value(QStringLiteral("frame")).toInt();
            if (frame < from || frame > to) out.append(v);
        }
        for (const QVariant &v : copied) {
            QVariantMap key = v.toMap();
            const int frame = clipFrame + key.value(QStringLiteral("frame")).toInt();
            if (frame >= frames) continue;
            key.insert(QStringLiteral("frame"), frame);
            out.append(key);
            ++pasted;
        }
        return keysByFrame(out);
    };
    QVariantMap editKeys = entry.value(ClipKey::editKeys).toMap();
    for (auto it = copiedKeys.begin(); it != copiedKeys.end(); ++it)
        editKeys.insert(it.key(), merge(editKeys.value(it.key()).toList(), it.value().toList()));
    const QVariantList opacity = merge(entry.value(ClipKey::opacityKf).toList(),
                                       m_keyframeClipboard.value(QStringLiteral("opacityKf")).toList());
    // An effect's copied keys go to the clip's effect of the same kind and place among its kind.
    QVariantMap effectKeys, effectMaskKeys;
    const QVariantList copiedEffects = m_keyframeClipboard.value(QStringLiteral("effectKeys")).toList();
    QHash<QString, int> seen;
    for (const QVariant &v : entry.value(ClipKey::videoEffects).toList()) {
        const QVariantMap state = v.toMap();
        const QString id = state.value(QStringLiteral("id")).toString();
        const int nth = seen[id]++;
        for (const QVariant &c : copiedEffects) {
            const QVariantMap copied = c.toMap();
            if (copied.value(QStringLiteral("id")).toString() != id || copied.value(QStringLiteral("nth")).toInt() != nth) continue;
            QVariantMap merged;
            const QVariantMap curves = copied.value(QStringLiteral("keys")).toMap(), own = state.value(QStringLiteral("keys")).toMap();
            for (auto it = curves.begin(); it != curves.end(); ++it)
                if (const QVariantList keys = merge(own.value(it.key()).toList(), it.value().toList()); !keys.isEmpty())
                    merged.insert(it.key(), keys);
            if (!merged.isEmpty()) effectKeys.insert(state.value(QStringLiteral("uid")).toString(), merged);
            // Its mask's keys go to a mask of the same shape (a polygon's corners, to one with as many).
            const QVariantMap mask = state.value(QStringLiteral("mask")).toMap();
            if (mask.isEmpty() || mask.value(QStringLiteral("shape")) != copied.value(QStringLiteral("maskShape"))) continue;
            QVariantMap mergedMask;
            const QVariantMap maskCurves = copied.value(QStringLiteral("maskKeys")).toMap(), ownMask = state.value(QStringLiteral("maskKeys")).toMap();
            for (auto it = maskCurves.begin(); it != maskCurves.end(); ++it) {
                if (it.key() == QLatin1String("points")
                    && it.value().toList().value(0).toMap().value(QStringLiteral("value")).toList().size() != mask.value(QStringLiteral("points")).toList().size())
                    continue;
                if (const QVariantList keys = merge(ownMask.value(it.key()).toList(), it.value().toList()); !keys.isEmpty())
                    mergedMask.insert(it.key(), keys);
            }
            if (!mergedMask.isEmpty()) effectMaskKeys.insert(state.value(QStringLiteral("uid")).toString(), mergedMask);
        }
    }
    if (!pasted) {
        setStatus(tr("%1 ends before the copied keyframes would start").arg(name));
        return false;
    }
    QVariantMap patch{{QStringLiteral("editKeys"), editKeys}};
    if (!m_keyframeClipboard.value(QStringLiteral("opacityKf")).toList().isEmpty())
        patch.insert(QStringLiteral("opacityKf"), opacity);
    if (!effectKeys.isEmpty()) patch.insert(QStringLiteral("videoEffectKeys"), effectKeys);
    if (!effectMaskKeys.isEmpty()) patch.insert(QStringLiteral("videoEffectMaskKeys"), effectMaskKeys);
    setEditProperties(track, index, patch);
    setStatus(tr("Pasted %n keyframe(s) onto %1", nullptr, pasted).arg(name));
    return true;
}

bool Backend::applyMotionPreset(int track, int index, const QString &preset, const QVariantMap &options) {
    if (m_exporting || track < 0 || index < 0) return false;
    viewTimeline();
    emit motionPresetRequested(track, index, preset, plainVariant(options).toMap());
    return true;
}

void Backend::beginEditGesture() { m_editGesture = ++m_editGestureSerial; }
void Backend::endEditGesture() { m_editGesture = 0; }

void Backend::setFrameBlend(int track, int index, int mode) {
    const auto refuse=[this](const QString &problem){setStatus(problem);emit failed(problem);};
    if (m_exporting) { refuse(tr("Cannot change frame sampling while exporting."));return; }
    if (track < 0 || index < 0) { refuse(tr("This clip is no longer available."));return; }
    if (!FrameSampling::valid(mode)) { refuse(tr("Choose a valid frame sampling mode."));return; }
    const bool mapped=entryOn(track,index).contains(ClipKey::retime);
    // Under colour management neither bakes yet: the timeline keeps playing
    // the nearest frame and an export stops on the clip, so say that now.
    QVariantList entries = m_timeline;
    entries.append(m_connected);
    const bool managed = FrameSampling::interpolates(mode) && !mapped && LinearRenderer::enabled(entries, m_projectSettings);
    setStatus(mode == FrameSampling::Floor ? tr("Floor — active in the viewer and export")
              : mode == FrameSampling::Nearest ? tr("Nearest frame — active in the viewer and export")
              : mapped && FrameSampling::interpolates(mode) ? (mode==2 ? tr("Optical flow — active in the viewer and export")
                                         : tr("Frame blending — active in the viewer and export"))
              : managed ? (mode == 2 ? tr("Optical flow — colour management is on, so the timeline plays the nearest frame and export stops on this clip")
                                   : tr("Frame blending — colour management is on, so the timeline plays the nearest frame and export stops on this clip"))
              : mode == 2 ? tr("Optical flow — bakes on Render in Place or export")
              : mode == 1 ? tr("Frame blending — bakes on Render in Place or export")
                          : tr("Original sampling — keeps the saved clip behaviour"));
    emit setFrameBlendRequested(track, index, mode);
}

void Backend::setConnectedRotation(int connectedIndex, double degrees) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (e.value(ClipKey::rotation).toDouble() == degrees)
        return;
    setStatus(tr("Rotation %1°").arg(degrees));
    emit setConnectedRotationRequested(connectedIndex, degrees);
}

void Backend::setFadeLengths(int timelineIndex, int fadeIn, int fadeOut) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size()
        || fadeIn < 0 || fadeOut < 0)
        return;
    const QVariantMap e = m_timeline.at(timelineIndex).toMap();
    if (e.value(ClipKey::fadeIn).toInt() == fadeIn
        && e.value(ClipKey::fadeOut).toInt() == fadeOut)
        return;
    setStatus(tr("Fades: %1 in, %2 out").arg(fadeIn).arg(fadeOut));
    emit setFadesRequested(timelineIndex, fadeIn, fadeOut);
}

void Backend::setConnectedFadeLengths(int connectedIndex, int fadeIn, int fadeOut) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size()
        || fadeIn < 0 || fadeOut < 0)
        return;
    const QVariantMap e = m_connected.at(connectedIndex).toMap();
    if (e.value(ClipKey::fadeIn).toInt() == fadeIn
        && e.value(ClipKey::fadeOut).toInt() == fadeOut)
        return;
    setStatus(tr("Fades: %1 in, %2 out").arg(fadeIn).arg(fadeOut));
    emit setConnectedFadesRequested(connectedIndex, fadeIn, fadeOut);
}

void Backend::setConnectedColor(int connectedIndex, double brightness,
                                double contrast, double saturation) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap entry = m_connected.at(connectedIndex).toMap();
    if (entry.value(ClipKey::colorB).toDouble() == brightness
        && entry.value(ClipKey::colorC, 1.0).toDouble() == contrast
        && entry.value(ClipKey::colorS, 1.0).toDouble() == saturation)
        return;
    emit setConnectedColorRequested(connectedIndex, brightness, contrast, saturation);
}

void Backend::setClipColor(int timelineIndex, double brightness,
                           double contrast, double saturation) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    if (entry.value(ClipKey::colorB).toDouble() == brightness
        && entry.value(ClipKey::colorC, 1.0).toDouble() == contrast
        && entry.value(ClipKey::colorS, 1.0).toDouble() == saturation)
        return; // a slider release with nothing changed is not an edit
    const bool neutral = brightness == 0.0 && contrast == 1.0
                         && saturation == 1.0;
    setStatus(neutral ? tr("Colour correction removed")
                      : tr("Colour corrected"));
    emit setColorRequested(timelineIndex, brightness, contrast, saturation);
}

void Backend::setClipSpeed(int timelineIndex, double speed) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const auto result=RetimeEdit::preview(m_timeline[timelineIndex].toMap(),speed);
    if (!result.value("valid").toBool()) { setStatus(result.value("error").toString()); return; }
    const double rate = qAbs(speed);
    if (speed == 1.0)
        setStatus(tr("Normal speed"));
    else if (rate < 1.0)
        setStatus((speed < 0.0 ? tr("Reverse slow motion %1%") : tr("Slow motion %1%"))
                      .arg(qRound(rate * 100)));
    else if (rate == 1.0)
        setStatus(tr("Reverse"));
    else
        setStatus((speed < 0.0 ? tr("Reverse fast %1×") : tr("Fast %1×")).arg(rate));
    emit setSpeedClipRequested(timelineIndex, speed);
}

void Backend::setConnectedSpeed(int connectedIndex, double speed) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const auto result=RetimeEdit::preview(m_connected[connectedIndex].toMap(),speed);
    if (!result.value("valid").toBool()) { setStatus(result.value("error").toString()); return; }
    const double rate = qAbs(speed);
    if (speed == 1.0)
        setStatus(tr("Normal speed"));
    else if (rate < 1.0)
        setStatus((speed < 0.0 ? tr("Reverse slow motion %1%") : tr("Slow motion %1%"))
                      .arg(qRound(rate * 100)));
    else if (rate == 1.0)
        setStatus(tr("Reverse"));
    else
        setStatus((speed < 0.0 ? tr("Reverse fast %1×") : tr("Fast %1×")).arg(rate));
    emit setSpeedConnectedRequested(connectedIndex, speed);
}

void Backend::setDissolveStyle(int timelineIndex, const QString &style) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    if (m_timeline.at(timelineIndex).toMap()
            .value(ClipKey::mixOut).toInt() < 1)
        return;
    QString shown = style;
    for (const QVariant &v : transitionStyles())
        if (v.toMap().value(QStringLiteral("key")).toString() == render::transitionBase(style)) shown = v.toMap().value(QStringLiteral("name")).toString();
    setStatus(tr("Transition: %1").arg(shown));
    emit restyleDissolveRequested(timelineIndex, style);
}

void Backend::resizeDissolve(int timelineIndex, int frames) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    const int current = entry.value(ClipKey::mixOut).toInt();
    if (current < 1 || frames == current)
        return;
    if (frames < 1) {
        emit removeDissolveRequested(timelineIndex);
        setStatus(tr("Dissolve removed"));
        return;
    }
    emit resizeDissolveRequested(timelineIndex, frames);
    setStatus(tr("Dissolve: %1").arg(Interchange::durationText(frames, fps())));
}

void Backend::toggleFade(int timelineIndex, bool outEdge) {
    if (timelineIndex < 0 || timelineIndex >= m_timeline.size())
        return;
    const QVariantMap entry = m_timeline.at(timelineIndex).toMap();
    int fadeIn = entry.value(ClipKey::fadeIn).toInt();
    int fadeOut = entry.value(ClipKey::fadeOut).toInt();
    const int second = qMax(2, qRound(m_fps)); // ~1 s
    if (outEdge)
        fadeOut = fadeOut > 0 ? 0 : second;
    else
        fadeIn = fadeIn > 0 ? 0 : second;
    // The edge to flip, not this mirror's two values: a second toggle before
    // the worker's echo must not undo the first (the status is a forecast).
    emit toggleFadeEdgeRequested(false, timelineIndex, outEdge, second);
    setStatus(outEdge ? (fadeOut > 0 ? tr("Fade out added")
                                     : tr("Fade out removed"))
                      : (fadeIn > 0 ? tr("Fade in added")
                                    : tr("Fade in removed")));
}

void Backend::toggleConnectedDissolve(int connectedIndex) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap entry = m_connected.at(connectedIndex).toMap();
    if (entry.value(ClipKey::mixOut).toInt() > 0) {
        emit removeConnectedDissolveRequested(connectedIndex);
        setStatus(tr("Dissolve removed"));
        return;
    }
    emit addTransitionRequested(entry.value(ClipKey::track).toInt(), entry.value(ClipKey::index).toInt(),
                                qMax(2, qRound(m_fps)), QString());
    setStatus(tr("Cross dissolve into the next clip on %1")
                  .arg(m_tracks.size() > entry.value(ClipKey::track).toInt()
                           ? m_tracks.at(entry.value(ClipKey::track).toInt())
                                 .toMap().value(QStringLiteral("name")).toString()
                           : QStringLiteral("the track")));
}

void Backend::resizeConnectedDissolve(int connectedIndex, int frames) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    emit resizeConnectedDissolveRequested(connectedIndex, frames);
}

void Backend::setConnectedDissolveStyle(int connectedIndex, const QString &style) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    emit restyleConnectedDissolveRequested(connectedIndex, style);
}

void Backend::toggleConnectedFade(int connectedIndex, bool outEdge) {
    if (connectedIndex < 0 || connectedIndex >= m_connected.size())
        return;
    const QVariantMap entry = m_connected.at(connectedIndex).toMap();
    int fadeIn = entry.value(ClipKey::fadeIn).toInt();
    int fadeOut = entry.value(ClipKey::fadeOut).toInt();
    const int second = qMax(2, qRound(m_fps));
    if (outEdge)
        fadeOut = fadeOut > 0 ? 0 : second;
    else
        fadeIn = fadeIn > 0 ? 0 : second;
    // The edge to flip, not this mirror's two values: a second toggle before
    // the worker's echo must not undo the first (the status is a forecast).
    emit toggleFadeEdgeRequested(true, connectedIndex, outEdge, second);
    setStatus(outEdge ? (fadeOut > 0 ? tr("Fade out added")
                                     : tr("Fade out removed"))
                      : (fadeIn > 0 ? tr("Fade in added")
                                    : tr("Fade in removed")));
}

void Backend::detachAudioOn(int track, int index) {
    if (m_exporting || track < 0)
        return;
    emit detachAudioOnRequested(track, index);
}

// ── Flip / freeze frame / reattach ───────────────────────────────────────

void Backend::reattachAudio(int track, int index) {
    if (m_exporting || track < 0)
        return;
    emit reattachAudioRequested(track, index);
    setStatus(tr("Reattached audio"));
}

void Backend::setFlip(int track, int index, bool h, bool v) {
    if (m_exporting || track < 0)
        return;
    emit setFlipRequested(track, index, h, v);
}

void Backend::setAudioMap(int track, int index, const QString &mode) {
    if (m_exporting || track < 0)
        return;
    emit setAudioMapRequested(track, index, mode);
}

void Backend::setNormalize(int track, int index, bool on) {
    if (m_exporting || track < 0)
        return;
    emit setNormalizeRequested(track, index, on);
    setStatus(on ? tr("Loudness normalised to −23 LUFS")
                 : tr("Loudness normalisation off"));
}

// The clip at the playhead: any track, the highest wins (the composite's
// rule); gaps and titles do not count.
QVariantMap Backend::clipUnderPlayhead() const {
    QVariantMap found;
    auto consider = [&](const QVariantMap &e, int track, int index) {
        if (e.value(ClipKey::gap).toBool()
            || !e.value(ClipKey::title).toString().isEmpty()
            || e.value(ClipKey::audioOnly).toBool())
            return;
        const int start = e.value(ClipKey::start).toInt();
        const int frames = e.value(ClipKey::frames).toInt();
        if (m_position < start || m_position >= start + frames)
            return;
        if (!found.isEmpty() && found.value(ClipKey::track).toInt() > track)
            return;
        found = e;
        found.insert(QStringLiteral("has"), true);
        found.insert(ClipKey::track, track);
        found.insert(ClipKey::index, index);
    };
    for (int i = 0; i < m_timeline.size(); ++i)
        consider(m_timeline.at(i).toMap(), 0, i);
    for (const QVariant &v : std::as_const(m_connected)) {
        const QVariantMap e = v.toMap();
        consider(e, e.value(ClipKey::track).toInt(),
                 e.value(ClipKey::index).toInt());
    }
    if (found.isEmpty())
        found.insert(QStringLiteral("has"), false);
    return found;
}

void Backend::freezeFrameAt(int track, int index, int frame) {
    if (m_exporting || track < 0 || !hasVideo())
        return;
    emit freezeFrameRequested(track, index, frame, qMax(1, qRound(m_fps * 2)));
    setStatus(tr("Freeze frame at %1").arg(timecode(frame)));
}

void Backend::freezeFrame() {
    const QVariantMap clip = clipUnderPlayhead();
    if (!clip.value(QStringLiteral("has")).toBool()) {
        setStatus(tr("No clip under the playhead to freeze."));
        return;
    }
    freezeFrameAt(clip.value(ClipKey::track).toInt(),
                  clip.value(ClipKey::index).toInt(), m_position);
}
