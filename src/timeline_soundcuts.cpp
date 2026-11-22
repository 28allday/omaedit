#include "timeline_p.h"

#include <QMutex>

namespace {
// A source's sound length in seconds, as its reader gives it. Every precise
// edit checks each cut of its track against its source: the length is kept
// for the file as it stands (its size and the time it was saved), so an edit
// opens only sources it has not seen.
bool soundSeconds(const QString &path, const AudioSampleSource::Options &options, double *seconds, QString *error) {
    static QMutex mutex;
    static QHash<QByteArray, double> known;
    const QFileInfo file(path);
    const QByteArray key = (path + QLatin1Char('#') + QString::number(options.streamIndex) + QLatin1Char('#') + QString::number(file.size())
                            + QLatin1Char('#') + QString::number(file.lastModified().toMSecsSinceEpoch())).toUtf8();
    const bool kept = file.exists();  // generators and the like are asked each time
    if (kept) {
        QMutexLocker lock(&mutex);
        if (const auto it = known.constFind(key); it != known.constEnd()) { *seconds = *it; return true; }
    }
    auto reader = AudioSampleSource::Reader::open(path, options, error);
    if (!reader) return false;
    *seconds = double(reader->sampleCount(48000)) / 48000.;
    if (kept) {
        QMutexLocker lock(&mutex);
        if (known.size() > 4096) known.clear();
        known.insert(key, *seconds);
    }
    return true;
}
}  // namespace

bool Timeline::prepareAudioTrack(int track, const QVariantList &clips, Track *prepared, QString *error) {
    if (error) error->clear();
    const auto refuse=[&](const char *text) {
        if (error) *error=QCoreApplication::translate("Timeline",text);
        return false;
    };
    if (track<1 || (clips.isEmpty() && !m_profile))
        return refuse(QT_TRANSLATE_NOOP("Timeline","Precise sound cuts need an existing timeline and audio track."));
    if (clips.isEmpty()) {
        prepared->playlist=std::make_unique<edit::Playlist>(*m_profile);
        prepared->kind=Track::Audio; return prepared->playlist->is_valid();
    }
    if (!m_profile && !lockProfileTo(clips.first().toMap().value(ClipKey::path).toString())) return false;
    const auto model=AudioTimeline::prepare(clips,m_profile->frame_rate_num(),m_profile->frame_rate_den(),error);
    if (!model) return false;
    // The cuts are the track (the mixer reads them); its playlist holds their
    // span. Each cut's source is checked as the mixer will read it.
    QHash<QByteArray,double> durations;  // seconds of sound, by source and stream
    QVariantList entries;
    long double end=0;
    const auto seconds=[](const AudioTimeline::Time &t){return static_cast<long double>(t.ticks)/t.rate;};
    for (const auto &cut:*model) {
        const auto &entry=cut.entry;
        const auto settings=m_mediaSources.value(entry.value(ClipKey::path).toString(),entry.value(ClipKey::sourceMeta)).toMap();
        // Unsupported processing must be explicit while the precise graph is
        // integrated. Never silently drop a saved effect or retime its samples.
        if (!qFuzzyCompare(qAbs(entry.value(ClipKey::speed,1.).toDouble()),1.) || !entry.value(ClipKey::retime).toMap().isEmpty()
            || !qFuzzyCompare(interpretationFactor(settings),1.) || entry.value(ClipKey::sourceOrigin).toDouble()!=0.
            || entry.value(ClipKey::fadeIn).toInt()!=0 || entry.value(ClipKey::fadeOut).toInt()!=0
            || !entry.value(ClipKey::volumeKf).toList().isEmpty() || !entry.value(ClipKey::eq).toString().isEmpty()
            || entry.value(ClipKey::normalize).toBool() || !entry.value(ClipKey::audioMap).toString().isEmpty()
            || entry.value(ClipKey::mixIn).toInt()!=0 || entry.value(ClipKey::mixOut).toInt()!=0
            || !entry.value(ClipKey::compoundJson).toString().isEmpty() || !entry.value(ClipKey::mcam).toString().isEmpty())
            return refuse(QT_TRANSLATE_NOOP("Timeline","This sound processing is not yet supported with precise cut timing."));
        const bool bypass=entry.value(ClipKey::bypass).toStringList().contains("level");
        const double volume=bypass?0.:entry.value(ClipKey::volume).toDouble(), pan=bypass?0.:entry.value(ClipKey::pan).toDouble();
        if (!std::isfinite(volume) || !std::isfinite(pan))
            return refuse(QT_TRANSLATE_NOOP("Timeline","The sound level or pan is invalid."));
        const auto playback=playbackPath(entry.value(ClipKey::path).toString());
        const bool offline=m_allowOffline && QFileInfo(playback).isAbsolute() && !QFileInfo::exists(playback);
        if (cut.start.rate<=0 || cut.in.rate<=0 || cut.duration.rate<=0 || cut.start.ticks<0 || cut.in.ticks<0 || cut.duration.ticks<=0)
            return refuse(QT_TRANSLATE_NOOP("AudioSequence","The sound cut has invalid timing or source media."));
        if (!offline) {
            AudioSampleSource::Options options; options.reverse=entry.value(ClipKey::speed,1.).toDouble()<0;
            options.streamIndex=entry.value("audioStream",-1).toInt();
            const QByteArray key=(playback+QLatin1Char('#')+QString::number(options.streamIndex)).toUtf8();
            if (!durations.contains(key)) {
                double length=0;
                if (!soundSeconds(playback,options,&length,error)) return false;
                durations.insert(key,length);
            }
            if (seconds(cut.in)+seconds(cut.duration)>durations.value(key)+1e-6L)
                return refuse(QT_TRANSLATE_NOOP("AudioSequence","The sound cut exceeds its available source or timeline length."));
        }
        if ((seconds(cut.start)+seconds(cut.duration))*m_profile->fps()>100000000)
            return refuse(QT_TRANSLATE_NOOP("AudioSequence","The sound cut exceeds its available source or timeline length."));
        end=std::max(end,seconds(cut.start)+seconds(cut.duration));
        auto saved=entry; saved.remove(ClipKey::offline); if(offline)saved[ClipKey::offline]=true;
        saved[ClipKey::track]=track; saved[ClipKey::lane]=track; saved[ClipKey::index]=entries.size(); entries.append(saved);
    }
    const int frames=std::max(1,int(std::ceil(end*m_profile->fps()-1e-10L)));
    edit::Producer sequence(*m_profile,"omaedit.preciseSound",frames);
    sequence.set("omavid:audioOnly",1);
    auto playlist=std::make_unique<edit::Playlist>(*m_profile);
    if (playlist->append(sequence)!=0)
        return refuse(QT_TRANSLATE_NOOP("Timeline","Could not prepare the precise sound track."));
    prepared->playlist=std::move(playlist); prepared->audioEntries=entries; prepared->kind=Track::Audio;
    return true;
}

bool Timeline::restoreAudioTracks(const QMap<int,QVariantList> &changes, QString *error) {
    if (error) error->clear();
    if (changes.isEmpty() || changes.firstKey()<1 || changes.lastKey()>kMaxTracks) return false;
    QVariantList identities;
    for (const auto &value:entries()) if (!changes.contains(value.toMap().value(ClipKey::track).toInt())) identities.append(value);
    for (const auto &cuts:changes) identities.append(cuts);
    if (!AudioTimeline::uniqueIds(identities)) {
        if(error)*error=QCoreApplication::translate("Timeline","The sound cut identity is missing or duplicated.");
        return false;
    }
    // Prepare every affected graph before touching the live tracks. A rejected
    // destination, source handle or effect cannot leave half of a move applied.
    std::vector<std::pair<int,Track>> prepared;
    for (auto it=changes.cbegin();it!=changes.cend();++it) {
        Track track;
        if (!prepareAudioTrack(it.key(),it.value(),&track,error)) return false;
        prepared.emplace_back(it.key(),std::move(track));
    }
    if (!ensureLane(changes.lastKey()-1)) return false;
    
    for (auto &item:prepared) {
        auto &target=m_tracks[size_t(item.first)];
        target.playlist=std::move(item.second.playlist); target.audioEntries=std::move(item.second.audioEntries);
        target.kind=Track::Audio;
    }
    refreshTrackFilters(); return true;
}

bool Timeline::restoreAudioTrack(int track, const QVariantList &clips, QString *error) {
    return !clips.isEmpty() && restoreAudioTracks({{track,clips}},error);
}

bool Timeline::setAudioTimingAt(int track, int index, const QVariantMap &timing, QString *error) {
    if (error) error->clear();
    if (track<1 || track>=trackCount() || trackLocked(track) || trackKind(track)!=Track::Audio || !m_profile) return false;
    AudioTimeline::Time start,in,duration;
    if (timing.size()!=3 || !AudioTimeline::readTime(timing.value(QStringLiteral("start")),&start)
        || !AudioTimeline::readTime(timing.value(QStringLiteral("in")),&in) || !AudioTimeline::readTime(timing.value("duration"),&duration)) return false;
    QVariantList cuts; bool found=false;
    for (const auto &v:entries()) {
        auto entry=v.toMap(); if (entry.value(ClipKey::lane).toInt()!=track) continue;
        entry=AudioTimeline::fromFrames(entry,m_profile->frame_rate_num(),m_profile->frame_rate_den());
        if (entry.isEmpty()) return false;
        if (entry.value(ClipKey::index).toInt()==index) {
            entry=AudioTimeline::timed(entry,start,in,duration,m_profile->frame_rate_num(),m_profile->frame_rate_den()); found=true;
        }
        if (entry.isEmpty()) return false;
        cuts.append(entry);
    }
    return found && restoreAudioTrack(track,cuts,error);
}
bool Timeline::splitAudioAt(int track, int index, const QVariantMap &position, QString *error) {
    return gestureAudioAt(track,index,"split",{{"position",position}},48000,nullptr,error);
}

bool Timeline::gestureAudioAt(int track, int index, const QString &action, const QVariantMap &changes,
                               int sampleRate, QVariantMap *selected, QString *error) {
    if (error) error->clear();
    if (selected) selected->clear();
    const int destination=changes.value("destinationTrack",track).toInt();
    if (!m_profile || track<1 || track>=trackCount() || destination<1 || destination>=trackCount()
        || trackKind(track)!=Track::Audio || trackKind(destination)!=Track::Audio
        || trackLocked(track) || trackLocked(destination) || (destination!=track && action!="move")) return false;
    QVariantMap chosen;
    QMap<int,QVariantList> tracks{{track,{}}};
    if (destination!=track) tracks[destination]={};
    for (const auto &value:entries()) {
        auto entry=value.toMap(); const int lane=entry.value(ClipKey::track).toInt();
        if (!tracks.contains(lane)) continue;
        entry=AudioTimeline::fromFrames(entry,m_profile->frame_rate_num(),m_profile->frame_rate_den());
        if (entry.isEmpty()) return false;
        if (lane==track && entry.value(ClipKey::index).toInt()==index) chosen=entry;
        else tracks[lane].append(entry);
    }
    if (chosen.isEmpty()) return false;
    const auto draft=AudioTimingEdit::gesture(chosen,m_profile->frame_rate_num(),m_profile->frame_rate_den(),action,changes,sampleRate);
    if (!draft.value("valid").toBool()) { if(error)*error=draft.value("error").toString();return false; }
    if (!draft.value("changed").toBool() && destination==track) return true;
    QVariantList replacements;
    if (action=="split") {
        AudioTimeline::Time at; AudioTimeline::readTime(draft.value("position"),&at);
        replacements=AudioTimeline::split(chosen,at,m_profile->frame_rate_num(),m_profile->frame_rate_den());
    } else replacements.append(draft.value("entry"));
    if (replacements.isEmpty()) return false;
    AudioTimeline::Time ripple,pivot;
    if (changes.value("ripple").toBool()) {
        if (!AudioTimeline::readTime(draft.value("rippleDelta"),&ripple)
            || !AudioTimeline::readTime(draft.value("ripplePivot"),&pivot)) return false;
        for (auto &value:tracks[track]) {
            const auto entry=value.toMap(); AudioTimeline::Time start,duration;
            if (!AudioTimeline::readSpan(entry,&start,&duration)) return false;
            if (__int128(start.ticks)*pivot.rate<__int128(pivot.ticks)*start.rate) continue;
            const auto shifted=AudioTimeline::shifted(entry,ripple,m_profile->frame_rate_num(),m_profile->frame_rate_den());
            if (shifted.isEmpty()) return false;
            value=shifted;
        }
    }
    tracks[destination].append(replacements);
    const auto id=replacements.first().toMap().value(ClipKey::clipId).toString();
    if (!restoreAudioTracks(tracks,error)) return false;
    if (selected) *selected=AudioTimeline::entryWithId(entries(),id);
    return true;
}
bool Timeline::pasteAudioClips(const QVariantList &copies,int sourceNum,int sourceDen,
                                const QVariantMap &position,bool insert,QVariantList *added,QString *error) {
    if(error)error->clear();
    if(added)added->clear();
    const auto invalid=[&] {
        if(error)*error=QCoreApplication::translate("AudioClipboard","The copied sound or destination timing is invalid.");
        return false;
    };
    AudioTimeline::Time at;
    if(!m_profile || !AudioTimeline::readTime(position,&at) || copies.isEmpty())return invalid();
    QMap<int,QVariantList> destinations;
    for(const auto &value:copies) {
        const auto entry=value.toMap();const int track=entry.value(ClipKey::track,-1).toInt();
        if(!entry.value(ClipKey::audioOnly).toBool() || track<1 || track>trackCount()
            || (track<trackCount() && (trackKind(track)!=Track::Audio || trackLocked(track))))return invalid();
        destinations[track]={};
    }
    for(const auto &value:entries()) {
        const int track=value.toMap().value(ClipKey::track).toInt();
        if(destinations.contains(track))destinations[track].append(value);
    }
    const auto prepared=AudioClipboard::prepare(copies,destinations,sourceNum,sourceDen,
        m_profile->frame_rate_num(),m_profile->frame_rate_den(),at,insert,error);
    if(!prepared || !restoreAudioTracks(prepared->tracks,error))return false;
    if(added) {
        const auto committed=entries();
        for(const auto &value:prepared->added)
            added->append(AudioTimeline::entryWithId(committed,value.toMap().value(ClipKey::clipId).toString()));
    }
    return true;
}

bool Timeline::pasteClipboardClips(const QVariantList &copies,int sourceNum,int sourceDen,
                                    const QVariantMap &position,QVariantList *added,QString *error,bool insert) {
    using Time=AudioTimeline::Time;
    if(error)error->clear();if(added)added->clear();
    const auto fail=[&](const char *text) {
        if(error)*error=QCoreApplication::translate("Timeline",text);
        return false;
    };
    const auto invalid=[&] {return fail(QT_TRANSLATE_NOOP("Timeline","The copied clips or destination timing are invalid."));};
    const auto compare=[](Time a,Time b){const auto d=__int128(a.ticks)*b.rate-__int128(b.ticks)*a.rate;return d<0?-1:d>0?1:0;};
    const auto subtract=[](Time a,Time b){return AudioTimeline::add(a,{-b.ticks,b.rate});};
    Time at,origin,end;
    if(copies.isEmpty() || sourceNum<1 || sourceDen<1
        || !AudioTimeline::readTime(position,&at) || at.ticks<0)return invalid();
    Timeline staged;staged.m_project=m_project;staged.m_mediaSources=m_mediaSources;
    staged.m_proxyMode=m_proxyMode;staged.m_allowOffline=m_allowOffline;
    staged.m_profile=m_profile;
    const auto firstCopy=copies.first().toMap();
    const auto firstPath=firstCopy.value(ClipKey::title).toString().isEmpty()
        ?firstCopy.value(ClipKey::path).toString():QStringLiteral("color:black");
    if(!staged.m_profile && !staged.lockProfileTo(firstPath))return invalid();
    const auto clock=staged.m_profile;
    const int num=clock->frame_rate_num(),den=clock->frame_rate_den();
    const auto frame=[&](Time time)->std::optional<int> {
        const auto n=__int128(time.ticks)*num,d=__int128(time.rate)*den;
        if(n<0 || d<=0 || n%d || n/d>100000000)return {};
        return int(n/d);
    };
    struct Picture {QVariantMap entry;Time start,duration;int track;};
    QList<Picture> pictures;QVariantList sound;
    QMap<int,QVariantList> soundTracks;QSet<int> pictureTracks;
    bool first=true;int lastTrack=0;QSet<int> newTracks;
    const int firstNewTrack=qMax(1,trackCount());
    for(const auto &value:copies) {
        const auto entry=value.toMap();const int track=entry.value(ClipKey::track,-1).toInt();
        const bool audio=entry.value(ClipKey::audioOnly).toBool();Time start,duration;
        if(value.metaType().id()!=QMetaType::QVariantMap || track<0
            || qint64(track)>=qint64(firstNewTrack)+copies.size() || (audio && track==0)
            || entry.value(ClipKey::lane,track).toInt()!=track || entry.value(ClipKey::gap).toBool())return invalid();
        if(track>=firstNewTrack)newTracks.insert(track);
        if(track<trackCount() && (trackLocked(track) || (trackKind(track)==Track::Audio)!=audio))
            return fail(QT_TRANSLATE_NOOP("Timeline","Choose unlocked tracks that match the copied picture and sound."));
        if(entry.contains(ClipKey::audioTiming)) {
            if(!audio || !AudioTimeline::readSpan(entry,&start,&duration))return invalid();
        } else {
            const auto s=entry.value(ClipKey::start).toLongLong(),length=entry.value(ClipKey::frames).toLongLong();
            if(s<0 || s>100000000 || length<1 || length>100000000
                || entry.value(ClipKey::start).toDouble()!=double(s) || entry.value(ClipKey::frames).toDouble()!=double(length))return invalid();
            start={s*sourceDen,sourceNum};duration={length*sourceDen,sourceNum};
        }
        const auto tail=AudioTimeline::add(start,duration);if(!tail)return invalid();
        if(first || compare(start,origin)<0)origin=start;
        if(first || compare(*tail,end)>0)end=*tail;
        first=false;lastTrack=qMax(lastTrack,track);
        if(audio){sound.append(entry);soundTracks[track]={};}
        else {pictures.append({entry,start,duration,track});pictureTracks.insert(track);}
    }
    // New destination lanes must be consecutive; a clipboard cannot allocate
    // an arbitrary sparse track number. Each added lane owns at least one copy.
    for(int track=firstNewTrack;track<=lastTrack;++track)if(!newTracks.contains(track))return invalid();
    for(int track:pictureTracks)if(soundTracks.contains(track))return invalid();
    const auto duration=subtract(end,origin),delta=subtract(at,origin);
    if(!duration || !delta)return invalid();
    const auto before=entries();
    const auto destinationPictures=pictureTracks;
    QSet<int> playlistTracks=pictureTracks;
    QMap<int,QVariantList> otherSound;
    QMap<int,QVariantList> shiftedPlaylists;
    QMap<int,int> pictureGaps;
    if(insert) {
        const auto spanFrames=frame(*duration);
        const auto pictureRefusal=[&] {
            return fail(QT_TRANSLATE_NOOP("Timeline","This insertion would move or cut pictures between frames. Use a frame-aligned span or turn off auto-selection for those picture tracks."));
        };
        const auto transitionRefusal=[&] {
            return fail(QT_TRANSLATE_NOOP("Timeline","This insertion crosses a transition. Remove the transition before inserting; nothing was changed."));
        };
        QHash<QString,bool> primaryMoves;
        for(const auto &value:before) {
            const auto e=value.toMap();const auto link=e.value(ClipKey::link).toString();
            if(e.value(ClipKey::track).toInt()!=0 || link.isEmpty())continue;
            const Time start{qint64(e.value(ClipKey::start).toInt())*den,num};
            const Time tail{(qint64(e.value(ClipKey::start).toInt())+e.value(ClipKey::frames).toInt())*den,num};
            primaryMoves[link]=!trackLocked(0) && (destinationPictures.contains(0) || m_tracks[0].autoSelect)
                && (compare(start,at)>=0 || (compare(tail,at)>0 && !e.value(ClipKey::gap).toBool()));
        }
        for(int track=0;track<trackCount();++track) {
            const bool destination=destinationPictures.contains(track) || soundTracks.contains(track);
            const auto &flags=m_tracks[size_t(track)];
            if(soundTracks.contains(track) || (!destination && (flags.locked || !flags.autoSelect)))continue;
            QVariantList current;bool changed=false,precise=false,crossesTransition=false;
            for(const auto &value:before)if(value.toMap().value(ClipKey::track).toInt()==track) {
                auto e=value.toMap();Time start,length;
                if(e.contains(ClipKey::audioTiming)) {
                    if(!AudioTimeline::readSpan(e,&start,&length))return invalid();
                    precise=true;
                } else {
                    start={qint64(e.value(ClipKey::start).toInt())*den,num};length={qint64(e.value(ClipKey::frames).toInt())*den,num};
                }
                const auto tail=AudioTimeline::add(start,length);if(!tail)return invalid();
                bool moves=compare(start,at)>=0;
                if(!destination && track>0 && !destinationPictures.contains(0)
                    && primaryMoves.contains(e.value(ClipKey::link).toString()))moves=moves && primaryMoves.value(e.value(ClipKey::link).toString());
                const bool crosses=compare(start,at)<0 && compare(*tail,at)>0;
                if(flags.kind==Track::Video && (track==0 || destination)) {
                    if(moves || crosses) {
                        if(!spanFrames || *spanFrames<1 || (crosses && !e.value(ClipKey::gap).toBool() && !frame(at)))return pictureRefusal();
                        changed=true;
                    }
                } else if(moves) {
                    if(flags.kind==Track::Video && (!spanFrames || *spanFrames<1))return pictureRefusal();
                    if(e.contains(ClipKey::audioTiming) || !spanFrames) {
                        const auto original=AudioTimeline::fromFrames(e,num,den);
                        e=AudioTimeline::shifted(original,*duration,num,den);if(e.isEmpty())return invalid();
                        precise=true;
                    } else e[ClipKey::start]=e.value(ClipKey::start).toInt()+*spanFrames;
                    changed=true;
                }
                // A transition cannot be torn by moving only one of its clips,
                // nor can an insertion split a transition's participating cut.
                if((crosses && (destination || track==0)) || (!moves && compare(*tail,at)>0))
                    if(e.value(ClipKey::mixIn).toInt()>0 || e.value(ClipKey::mixOut).toInt()>0)crossesTransition=true;
                current.append(e);
            }
            if(!changed)continue;
            if(crossesTransition)return transitionRefusal();
            if(flags.kind==Track::Video && (track==0 || destination)) {
                // In a gap, any frame boundary before the next cut yields the
                // same layout. A real cut above already required an exact pivot.
                const auto n=__int128(at.ticks)*num,d=__int128(at.rate)*den;
                if(n/d>100000000)return invalid();
                pictureGaps[track]=int((n+d-1)/d);
                playlistTracks.insert(track);pictureTracks.insert(track);
            } else if(flags.kind==Track::Audio && precise) {
                for(auto &value:current) {
                    value=AudioTimeline::fromFrames(value.toMap(),num,den);if(value.toMap().isEmpty())return invalid();
                }
                otherSound[track]=current;
            } else {
                shiftedPlaylists[track]=current;playlistTracks.insert(track);
                if(flags.kind==Track::Video)pictureTracks.insert(track);
            }
        }
    }
    for(const auto &value:before) {
        const int track=value.toMap().value(ClipKey::track).toInt();
        if(soundTracks.contains(track))soundTracks[track].append(value);
    }
    std::optional<AudioClipboard::Placement> audio;
    if(!sound.isEmpty()) {
        audio=AudioClipboard::prepare(sound,soundTracks,sourceNum,sourceDen,num,den,at,insert,error,AudioClipboard::Span{origin,*duration});
        if(!audio)return false;
    }
    if(!otherSound.isEmpty()) {
        if(!audio)audio=AudioClipboard::Placement{};
        for(auto it=otherSound.cbegin();it!=otherSound.cend();++it)audio->tracks[it.key()]=it.value();
    }
    QHash<QString,QString> links=audio?audio->links:QHash<QString,QString>{};
    // This staging timeline shares only the immutable profile, never live
    // producers. Its playlists and owner-bound meter filters are independent.
    if(staged.m_tracks.empty()) {
        Track primaryTrack;primaryTrack.playlist=std::make_unique<edit::Playlist>(*clock);
        staged.m_tracks.push_back(std::move(primaryTrack));
    }
    QVariantList pictureBefore;
    for(const auto &value:before) {
        const int track=value.toMap().value(ClipKey::track).toInt();
        if(playlistTracks.contains(track) && !shiftedPlaylists.contains(track))pictureBefore.append(value);
    }
    for(const auto &cuts:shiftedPlaylists)pictureBefore.append(cuts);
    std::stable_sort(pictureBefore.begin(),pictureBefore.end(),[](const QVariant &a,const QVariant &b){
        const auto x=a.toMap(),y=b.toMap();return x.value(ClipKey::track).toInt()!=y.value(ClipKey::track).toInt()
            ? x.value(ClipKey::track).toInt()<y.value(ClipKey::track).toInt() : x.value(ClipKey::start).toInt()<y.value(ClipKey::start).toInt();
    });
    if(!playlistTracks.isEmpty()) {
        if(!staged.restoreClips(pictureBefore))
            return fail(QT_TRANSLATE_NOOP("Timeline","The destination picture track could not be prepared; nothing was changed."));
        if(!pictureTracks.contains(0) && staged.clipCount()==0)
            if(!staged.append("color:black",0,qMax(1,frameCount())-1))return invalid();
        staged.applyTrackFlags(trackFlags());
        if(lastTrack>=staged.trackCount() && !staged.ensureLane(lastTrack-1))return invalid();
    }
    for(auto it=pictureGaps.cbegin();it!=pictureGaps.cend();++it) {
        const int track=it.key(),pivot=it.value();auto *playlist=staged.m_tracks[size_t(track)].playlist.get();
        if(pivot>=playlist->get_playtime())continue;
        int entry=playlist->get_clip_index_at(pivot);
        if(playlist->is_mix(entry) || (entry>0 && playlist->is_mix(entry-1)))return invalid();
        if(pivot>playlist->clip_start(entry)) {
            if(playlist->is_blank(entry)) {
                if(playlist->split(entry,pivot-playlist->clip_start(entry)-1)!=0)return invalid();
            } else {
                QScopedValueRollback<bool> linking(staged.m_linkingSplit,true);
                if(!staged.splitOn(track,pivot))return invalid();
            }
            entry=playlist->get_clip_index_at(pivot);
        }
        playlist->insert_blank(entry,*frame(*duration)-1);
    }
    QList<QPair<int,int>> pictureAddresses;
    std::stable_sort(pictures.begin(),pictures.end(),[&](const Picture &a,const Picture &b){
        return a.track!=b.track?a.track<b.track:compare(a.start,b.start)<0;
    });
    for(const auto &picture:pictures) {
        const auto start=AudioTimeline::add(picture.start,*delta);
        const auto tail=start?AudioTimeline::add(*start,picture.duration):std::nullopt;
        const auto from=start?frame(*start):std::nullopt,to=tail?frame(*tail):std::nullopt;
        if(!from || !to || *to<=*from)
            return fail(QT_TRANSLATE_NOOP("Timeline","This paste would place a picture boundary between frames. Choose a frame-aligned position or copy the sound separately."));
        auto entry=picture.entry;entry[ClipKey::timelineFps]=double(sourceNum)/sourceDen;
        if(!entry.value(ClipKey::videoOnly).toBool() && staged.pathHasAudio(entry.value(ClipKey::path).toString()))
            return fail(QT_TRANSLATE_NOOP("Timeline","Copy the picture and its sound together for precise clipboard placement; embedded sound cannot be placed separately yet."));
        if((!insert && !entry.value(ClipKey::title).toString().isEmpty()) || !entry.value(ClipKey::compoundJson).toString().isEmpty())
            return fail(QT_TRANSLATE_NOOP("Timeline","Mixed precise clipboard placement for titles and nested clips is not supported yet."));
        const auto metadata=m_mediaSources.value(entry.value(ClipKey::path).toString(),entry.value(ClipKey::sourceMeta)).toMap();
        entry=TimelineRate::convert(entry,double(num)/den,double(origin.ticks)/origin.rate,error,interpretationFactor(metadata));
        if(entry.isEmpty())return false;
        if(entry.value(ClipKey::frames).toInt()!=*to-*from || entry.value(ClipKey::mixIn).toInt()>0 || entry.value(ClipKey::mixOut).toInt()>0)
            return fail(QT_TRANSLATE_NOOP("Timeline","The copied picture transition or duration cannot be placed exactly yet."));
        const auto link=entry.value(ClipKey::link).toString();
        if(!link.isEmpty()) {
            if(!links.contains(link))links[link]=newLinkId();
            entry[ClipKey::link]=links[link];
        }
        if(!staged.pasteClip(picture.track,*from,entry,false))
            return fail(QT_TRANSLATE_NOOP("Timeline","The copied picture could not be placed; nothing was changed."));
        pictureAddresses.append({picture.track,*from});
    }
    std::vector<std::pair<int,Track>> preparedAudio;
    if(audio)for(auto it=audio->tracks.cbegin();it!=audio->tracks.cend();++it) {
        Track track;if(!staged.prepareAudioTrack(it.key(),it.value(),&track,error))return false;
        preparedAudio.emplace_back(it.key(),std::move(track));
    }
    // Every fallible media operation is complete.
    if(!m_profile) {
        m_profile=clock;m_rebuildProfileValid=true;settleStartFrame();
        Track primaryTrack;primaryTrack.playlist=std::make_unique<edit::Playlist>(*clock);
        m_tracks.push_back(std::move(primaryTrack));
    }
    if(lastTrack>=trackCount() && !ensureLane(lastTrack-1))return invalid();
    
    for(int track:playlistTracks) {
        auto &target=m_tracks[size_t(track)],&prepared=staged.m_tracks[size_t(track)];
        target.playlist=std::move(prepared.playlist);target.audioEntries.clear();target.kind=prepared.kind;
        // The staging destructor may still iterate its tracks in future code.
        prepared.playlist=std::make_unique<edit::Playlist>(*m_profile);
    }
    for(auto &item:preparedAudio) {
        auto &target=m_tracks[size_t(item.first)];target.playlist=std::move(item.second.playlist);
        target.audioEntries=std::move(item.second.audioEntries);target.kind=Track::Audio;
    }
    m_mediaSources=staged.m_mediaSources;
    refreshTrackFilters();
    if(added) {
        for(const auto &address:pictureAddresses)added->append(entryMapOn(address.first,clipIndexAt(address.first,address.second)));
        if(audio)for(const auto &value:audio->added)added->append(AudioTimeline::entryWithId(entries(),value.toMap().value(ClipKey::clipId).toString()));
    }
    return true;
}

bool Timeline::setAudioEntry(int track, int index, const QVariantMap &changes) {
    if (track<1 || track>=trackCount() || trackLocked(track)) return false;
    auto cuts=m_tracks[size_t(track)].audioEntries;
    if (index<0 || index>=cuts.size()) return false;
    auto entry=cuts[index].toMap(); for(auto it=changes.cbegin();it!=changes.cend();++it)entry[it.key()]=it.value();
    if(changes.contains("volume") || changes.contains("pan")) {
        auto bypass=entry.value(ClipKey::bypass).toStringList();bypass.removeAll("level");entry[ClipKey::bypass]=bypass;
    }
    cuts[index]=entry; return restoreAudioTrack(track,cuts);
}
