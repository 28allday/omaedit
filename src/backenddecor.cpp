#include "backend_p.h"

bool Backend::wantsTimelineDecor(const QVariantMap &entry) const {
    return !m_decorationWindowSet || m_decorationAddresses.contains(QString::number(entry.value(ClipKey::track).toInt())+':'+QString::number(entry.value(ClipKey::index).toInt()));
}
void Backend::scheduleDecorChanged() {
    if(m_decorNotifyPending)return;m_decorNotifyPending=true;
    QTimer::singleShot(16,this,[this]{m_decorNotifyPending=false;++m_decorationRevision;emit decorChanged();});
}
void Backend::pruneTimelineDecor() {
    m_decorationAddresses.clear();QSet<QString> images;bool changed=false;
    // Every clip in the window wide enough to show a strip or a wave keeps
    // it. There was a count here once (the first 128 in list order, V1
    // first): on a long edit V1 took it all and every sound, the Audio
    // page's only clips, stayed a flat line. The window and the width bound
    // the number by themselves: at most a clip every four pixels a track.
    auto prune=[&](QVariantList &entries,QVariantList &decor){
        const auto blocks=MixShape::blocks(entries); // the blocks as drawn, as the worker plans them
        for(int i=0;i<entries.size();++i){
            auto e=entries[i].toMap();const int start=e.value(ClipKey::start).toInt()-blocks[i].lead,frames=e.value(ClipKey::frames).toInt()+blocks[i].lead-blocks[i].tail;
            const bool keep=!m_decorationWindowSet || (m_decorationPixels>0 && !e.value(ClipKey::gap).toBool()
                && m_decorationLast>m_decorationFirst && start<m_decorationLast && double(start)+frames>m_decorationFirst
                && double(frames)*m_decorationPixels/qMax(1.,m_decorationLast-m_decorationFirst)>=4.);
            if(keep){m_decorationAddresses.insert(QString::number(e.value(ClipKey::track).toInt())+':'+QString::number(e.value(ClipKey::index).toInt()));}
            else {if(!decor.value(i).toMap().isEmpty()){decor[i]=QVariantMap();changed=true;}e.remove("wave");e.remove("strip");entries[i]=e;}
            if(keep)for(const auto &url:decor.value(i).toMap().value("strip").toStringList())images.insert(url.mid(15).section('?',0,0));
        }
    };
    prune(m_timeline,m_timelineDecor);prune(m_connected,m_connectedDecor);
    // Library/source strips remain available to their virtualized browsers;
    // the provider has its own byte limit and disk backing for these.
    for(const auto &value:m_library)for(const auto *field:{"strip","sourceStrip"})for(const auto &url:value.toMap().value(field).toStringList())images.insert(url.mid(15).section('?',0,0));
    if(m_thumbs)m_thumbs->retainStrips(images);
    if(changed)scheduleDecorChanged();
}
void Backend::setTimelineDecorationWindow(double first, double last, int pixels) {
    if(!std::isfinite(first)||!std::isfinite(last))return;
    first=qMax(0.,first);last=qMax(first,last);pixels=qBound(0,pixels,16384);
    if(m_decorationWindowSet && first==m_decorationFirst && last==m_decorationLast && pixels==m_decorationPixels)return;
    m_decorationWindowSet=true;m_decorationFirst=first;m_decorationLast=last;m_decorationPixels=pixels;
    pruneTimelineDecor();emit decorationWindowRequested(first,last,pixels);
}

void Backend::onTimeline(const QVariantList &entries, int duration, double fps,
                         int width, int height, bool canUndo, bool canRedo, bool contentEdit, const QVariantMap &format,
                         const QVariantMap &audioEditContext) {
    if (!AudioTimeline::sameEditContext(m_audioEditContext,audioEditContext,false)) cancelAudioSeek(true);
    m_audioEditContext=audioEditContext;
    m_timelineFormat = format;
    // Entries carry both lanes; QML wants them apart. Storyline entries come
    // first, so m_timeline's indices still equal playlist indices.
    const QVariantList oldTimeline = m_timeline, oldConnected = m_connected;
    const QVariantList oldTimelineDecor = m_timelineDecor, oldConnectedDecor = m_connectedDecor;
    m_timeline.clear();
    m_connected.clear();
    for (QVariant v : entries) {
        nameNestedClip(v, m_sequences);
        if (v.toMap().value(ClipKey::lane).toInt() >= 1)
            m_connected.append(v);
        else
            m_timeline.append(v);
    }
    // Decor: an entry that is still the same piece of media at the same
    // place keeps its strips and waves (a grade, a volume nudge, a label
    // must not blank every thumbnail); anything else starts empty and the
    // worker's requeued jobs fill it through decorChanged.
    auto sameMedia = [](const QVariantMap &a, const QVariantMap &b) {
        // A picture and its own sound share file, place and span: the kind
        // tells them apart, or one would wear the other's strips or wave.
        for (const char *k : {"path", "start", "in", "frames", "title", "compoundJson", "audioTiming", "audioStream", "audioOnly", "videoOnly"})
            if (a.value(QLatin1String(k)) != b.value(QLatin1String(k)))
                return false;
        return true;
    };
    auto mediaKey = [](const QVariantMap &e) {
        QVariantList values;
        for (const char *key : {"path", "start", "in", "frames", "title", "compoundJson", "audioTiming", "audioStream", "audioOnly", "videoOnly"}) values.append(e.value(QLatin1String(key)));
        return QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact);
    };
    auto carry = [&](const QVariantList &now, const QVariantList &before, const QVariantList &beforeDecor) {
        // The table of every earlier clip by its media (JSON of each, with a
        // compound's whole list) is made only when a clip is not where it
        // was: a grade, a level, a label or a trim at the end of the list
        // never needs it, and most edits are those.
        QHash<QByteArray,QVariant> byMedia;
        bool tabled=false;
        QVariantList decor;decor.reserve(now.size());
        for (int i=0;i<now.size();++i) {
            const auto e=now[i].toMap();
            if (i<before.size() && i<beforeDecor.size() && sameMedia(e,before[i].toMap())) { decor.append(beforeDecor[i]); continue; }
            if (!std::exchange(tabled,true))
                for (int j=0;j<before.size() && j<beforeDecor.size();++j) {
                    const auto key=mediaKey(before[j].toMap());
                    if (!byMedia.contains(key)) byMedia.insert(key,beforeDecor[j]);
                }
            decor.append(byMedia.value(mediaKey(e),QVariantMap()));
        }
        return decor;
    };
    m_timelineDecor = carry(m_timeline, oldTimeline, oldTimelineDecor);
    m_connectedDecor = carry(m_connected, oldConnected, oldConnectedDecor);
    pruneTimelineDecor();
    m_canUndo = canUndo;
    m_canRedo = canRedo;
    m_duration = duration;
    if (!m_audioPosition.isEmpty() && audioPictureFrame(m_audioPosition)<0) m_audioPosition.clear();
    if (!m_requestedAudioPosition.isEmpty() && audioPictureFrame(m_requestedAudioPosition)<0) cancelAudioSeek(false);
    emit audioPositionChanged();
    if (duration == 0) {
        // No frame will ever answer a seek on an empty timeline.
        m_seekInFlight = false;
        m_pendingSeek = -1;
    }
    m_fps = fps;
    m_videoWidth = width;
    m_videoHeight = height;
    const int lastPlace = duration > 0 ? duration - 1 + m_pastEndFrames : 0;
    if (m_position > lastPlace) {
        m_position = lastPlace;
        emit positionChanged();
    }
    emit timelineEntriesChanged();
    ++m_decorationRevision;emit decorChanged();
    emit videoChanged();
    updateMulticamClip();
    // Every timeline change dirties the project; a load announces first and
    // the projectLoaded handler wipes the flag straight after.
    if (contentEdit) { m_loudnessPass->invalidate(); setDirty(true, true); }
}

void Backend::onFilmstrip(int index, int start, const QStringList &keys,
                          const QList<QImage> &tiles, const QVariantMap &layout) {
    if (!m_thumbs || index < 0 || index >= m_timeline.size())
        return;
    QVariantMap entry = m_timeline.at(index).toMap();
    // The timeline may have been edited since the worker took this job.
    if (!wantsTimelineDecor(entry) || entry.value(ClipKey::start).toInt() != start
        || entry.value(ClipKey::frames) != layout.value("frames")
        || entry.value(ClipKey::in) != layout.value("in") || entry.value(ClipKey::path) != layout.value("path"))
        return;
    QStringList urls;
    for (int i = 0; i < keys.size() && i < tiles.size(); ++i) {
        if (tiles.at(i).isNull())
            continue;
        m_thumbs->insert(keys.at(i), tiles.at(i));
        urls.append(QStringLiteral("image://thumbs/") + keys.at(i) + QLatin1Char('?') + QString::number(++m_thumbKey));
    }
    entry.insert(QStringLiteral("strip"), urls);
    m_timeline[index] = entry; // C++ readers see it; QML reads the decor
    QVariantMap decor = m_timelineDecor.value(index).toMap();
    decor.insert(QStringLiteral("strip"), urls);
    // Fractions of the block as it is drawn (MixShape), which the strip was planned over.
    decor.insert(QStringLiteral("stripFirst"), layout.value("first").toDouble() / qMax(1, layout.value("shown").toInt()));
    decor.insert(QStringLiteral("stripStep"), layout.value("step").toDouble() / qMax(1, layout.value("shown").toInt()));
    m_timelineDecor[index] = decor;
    scheduleDecorChanged();
}

void Backend::onWaveform(int index, int start, const QVariantMap &wave) {
    if (index < 0 || index >= m_timeline.size())
        return;
    QVariantMap entry = m_timeline.at(index).toMap();
    if(!wantsTimelineDecor(entry))return;
    // The timeline may have been edited since the worker took this job.
    if (!wantsTimelineDecor(entry) || entry.value(ClipKey::start).toInt() != start)
        return;
    entry.insert(QStringLiteral("wave"), wave);
    m_timeline[index] = entry;
    QVariantMap decor = m_timelineDecor.value(index).toMap();
    decor.insert(QStringLiteral("wave"), wave);
    m_timelineDecor[index] = decor;
    scheduleDecorChanged();
}

void Backend::onConnectedFilmstrip(int index, int start, const QStringList &keys,
                                   const QList<QImage> &tiles, const QVariantMap &layout) {
    if (!m_thumbs || index < 0 || index >= m_connected.size())
        return;
    QVariantMap entry = m_connected.at(index).toMap();
    if (!wantsTimelineDecor(entry) || entry.value(ClipKey::start).toInt() != start
        || entry.value(ClipKey::audioOnly).toBool() // a sound has no pictures
        || entry.value(ClipKey::frames) != layout.value("frames")
        || entry.value(ClipKey::in) != layout.value("in") || entry.value(ClipKey::path) != layout.value("path"))
        return; // the lane was edited since the worker took this job
    QStringList urls;
    for (int i = 0; i < keys.size() && i < tiles.size(); ++i) {
        if (tiles.at(i).isNull())
            continue;
        m_thumbs->insert(keys.at(i), tiles.at(i));
        urls.append(QStringLiteral("image://thumbs/") + keys.at(i) + QLatin1Char('?') + QString::number(++m_thumbKey));
    }
    entry.insert(QStringLiteral("strip"), urls);
    m_connected[index] = entry;
    QVariantMap decor = m_connectedDecor.value(index).toMap();
    decor.insert(QStringLiteral("strip"), urls);
    // Fractions of the block as it is drawn (MixShape), which the strip was planned over.
    decor.insert(QStringLiteral("stripFirst"), layout.value("first").toDouble() / qMax(1, layout.value("shown").toInt()));
    decor.insert(QStringLiteral("stripStep"), layout.value("step").toDouble() / qMax(1, layout.value("shown").toInt()));
    m_connectedDecor[index] = decor;
    scheduleDecorChanged();
}

void Backend::onConnectedWaveform(int index, int start, const QVariantMap &wave) {
    if (index < 0 || index >= m_connected.size())
        return;
    QVariantMap entry = m_connected.at(index).toMap();
    // The lane may have been edited since the worker took this job; a
    // picture whose sound sits on its own track has no wave of its own.
    if (!wantsTimelineDecor(entry) || entry.value(ClipKey::start).toInt() != start
        || entry.value(ClipKey::videoOnly).toBool())
        return;
    entry.insert(QStringLiteral("wave"), wave);
    m_connected[index] = entry;
    QVariantMap decor = m_connectedDecor.value(index).toMap();
    decor.insert(QStringLiteral("wave"), wave);
    m_connectedDecor[index] = decor;
    scheduleDecorChanged();
}

void Backend::onSourceStrip(const QString &path, const QStringList &keys,
                            const QList<QImage> &tiles) {
    if (!m_thumbs)
        return;
    for (int i = 0; i < m_library.size(); ++i) {
        QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("path")).toString() != path)
            continue;
        QStringList urls;
        for (int j = 0; j < keys.size() && j < tiles.size(); ++j) {
            if (tiles.at(j).isNull())
                continue;
            m_thumbs->insert(keys.at(j), tiles.at(j));
            urls.append(QStringLiteral("image://thumbs/") + keys.at(j) + '?' + QString::number(++m_thumbKey));
        }
        entry.insert(QStringLiteral("sourceStrip"), urls);
        m_library[i] = entry;
        announceLibraryKeepingSources(); // a thumbnail strip: nothing the player reads
        return;
    }
}

void Backend::onLibraryStrip(const QString &path, const QStringList &keys,
                             const QList<QImage> &tiles) {
    if (!m_thumbs)
        return;
    bool touched = false;
    for (int i = 0; i < m_library.size(); ++i) {
        QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("path")).toString() != path)
            continue;
        QStringList urls;
        for (int j = 0; j < keys.size() && j < tiles.size(); ++j) {
            if (tiles.at(j).isNull())
                continue;
            m_thumbs->insert(keys.at(j), tiles.at(j));
            urls.append(QStringLiteral("image://thumbs/") + keys.at(j) + '?' + QString::number(++m_thumbKey));
        }
        entry.insert(QStringLiteral("strip"), urls);
        m_library[i] = entry;
        touched = true;
    }
    if (touched)
        announceLibraryKeepingSources();
}

void Backend::onLibraryWave(const QString &path, const QVariantMap &wave) {
    m_recentLibraryWaves.removeAll(path);m_recentLibraryWaves.prepend(path);
    while(m_recentLibraryWaves.size()>64){
        const auto expired=m_recentLibraryWaves.takeLast();
        for(int i=0;i<m_library.size();++i){auto entry=m_library[i].toMap();if(entry.value("path")==expired){entry.remove("wave");m_library[i]=entry;}}
    }
    bool touched = false;
    for (int i = 0; i < m_library.size(); ++i) {
        QVariantMap entry = m_library.at(i).toMap();
        if (entry.value(QStringLiteral("path")).toString() != path)
            continue;
        entry.insert(QStringLiteral("wave"), wave);
        m_library[i] = entry;
        touched = true;
    }
    if (touched)
        announceLibraryKeepingSources();
}
