#include "backend_p.h"

QVariantMap Backend::audioEditTarget(int track, int index) const {
    if(projectBusy() || m_exporting || m_audioEditContext.value("sequenceId").toString().isEmpty()
        || m_audioEditContext.value("sequenceId")!=m_sequences.value(m_activeSequence).toMap().value("id"))return {};
    const auto entry=entryOn(track,index); const auto id=entry.value(ClipKey::clipId).toString();
    if(entry.isEmpty() || !entry.value(ClipKey::audioOnly).toBool() || track<1
        || (!id.isEmpty() && AudioTimeline::entryWithId(m_connected,id).isEmpty()))return {};
    return {{"context",m_audioEditContext},{"clipId",id},{"track",track},{"index",index},
            {"entry",entry},{"state",AudioTimeline::editState(entry)}};
}
QString Backend::audioEditProblem(const QVariantMap &input) const {
    const auto target = plainVariant(input).toMap();
    const auto id = target.value("clipId").toString();
    if (projectBusy() || m_exporting || !AudioTimeline::sameEditContext(
            target.value("context").toMap(), m_audioEditContext, id.isEmpty()))
        return tr("The project or timeline changed. Select the sound cut again.");
    const auto entry = id.isEmpty() ? entryOn(target.value("track", -1).toInt(), target.value("index", -1).toInt())
                                   : AudioTimeline::entryWithId(m_connected, id);
    const int track = entry.value(ClipKey::track, -1).toInt();
    if (entry.isEmpty() || !entry.value(ClipKey::audioOnly).toBool() || track < 1 || track >= m_tracks.size()
        || m_tracks[track].toMap().value("locked").toBool() || AudioTimeline::editState(entry) != target.value("state").toString())
        return tr("The sound cut changed, is locked or is no longer available. Select it again.");
    return {};
}
QVariantMap Backend::audioTimingFields(const QVariantMap &timing, const QString &unit, int sampleRate) const {
    return AudioTimingEdit::fields(plainVariant(timing).toMap(), unit, sampleRate);
}
QVariantMap Backend::previewAudioTiming(const QVariantMap &input, const QVariantMap &fields,
                                       const QString &unit, int sampleRate) const {
    const auto target = plainVariant(input).toMap();
    auto error = audioEditProblem(target);
    if (error.isEmpty() && sampleRate != m_projectSettings.value("sampleRate", 48000).toInt())
        error = tr("The project sample rate changed. Reopen the sound timing controls.");
    if (!error.isEmpty()) return {{"valid", false}, {"error", error}};
    const auto context = target.value("context").toMap();
    return AudioTimingEdit::preview(target.value("entry").toMap(), context.value("fpsNum").toInt(),
        context.value("fpsDen").toInt(), plainVariant(fields).toMap(), unit, sampleRate);
}
int Backend::requestAudioEdit(const QVariantMap &input,const QVariantMap &inputValues,bool split) {
    const int request=++m_audioEditRequest;
    const auto target=plainVariant(input).toMap(),values=plainVariant(inputValues).toMap();
    QString error;
    if(projectBusy() || m_exporting || !AudioTimeline::sameEditContext(target.value("context").toMap(),m_audioEditContext,target.value("clipId").toString().isEmpty()))
        error=tr("The project or timeline changed. Select the sound cut again.");
    if(!error.isEmpty())QTimer::singleShot(0,this,[this,request,error] {setStatus(error);emit audioEditFinished(request,false,error);});
    else emit audioEditRequested(request,target,values,split);
    return request;
}
int Backend::setAudioTiming(const QVariantMap &target,const QVariantMap &timing) {return requestAudioEdit(target,timing,false);}
int Backend::splitAudio(const QVariantMap &target,const QVariantMap &position) {return requestAudioEdit(target,position,true);}

QVariantMap Backend::audioSnapPoints(const QVariantMap &input, double firstFrame, double lastFrame) const {
    return audioSnapPoints(input, firstFrame, lastFrame, true);
}

QVariantMap Backend::audioSnapPoints(const QVariantMap &input, double firstFrame, double lastFrame, bool beats) const {
    const auto exclude=plainVariant(input).toMap();
    const int num=m_audioEditContext.value("fpsNum").toInt(),den=m_audioEditContext.value("fpsDen").toInt();
    if (num<1 || den<1 || !std::isfinite(firstFrame) || !std::isfinite(lastFrame) || lastFrame<firstFrame) return {};
    QVariantList points;
    const auto append=[&](AudioTimeline::Time time,bool cursor=false) {
        const double frame=double(time.ticks)/time.rate*num/den;
        if (frame>=firstFrame && frame<=lastFrame) points.append(QVariantMap{{"time",AudioTimeline::writeTime(time)},{"frame",frame},{"cursor",cursor}});
    };
    append({0,1}); append({qint64(m_duration)*den,num});
    AudioTimeline::Time cursor; if (AudioTimeline::readTime(audioPosition(),&cursor)) append(cursor,true);
    for (const auto &marker:m_markers) append({qint64(marker.toMap().value("frame").toInt())*den,num});
    // The beats of mapped music, as on the Edit page.
    if (beats) for (const auto &beat:beatFrames()) append({qint64(beat.toInt())*den,num});
    const auto id=exclude.value("clipId").toString();
    for (const auto &value:m_connected) {
        const auto entry=value.toMap(); if (!entry.value(ClipKey::audioOnly).toBool()) continue;
        if (!exclude.isEmpty() && ((!id.isEmpty() && id==entry.value(ClipKey::clipId).toString())
            || (id.isEmpty() && entry.value(ClipKey::track)==exclude.value("track") && entry.value(ClipKey::index)==exclude.value("index")))) continue;
        AudioTimeline::Time start,duration;
        if (entry.contains(ClipKey::audioTiming)) {if(!AudioTimeline::readSpan(entry,&start,&duration))continue;}
        else {start={qint64(entry.value(ClipKey::start).toInt())*den,num};duration={qint64(entry.value(ClipKey::frames).toInt())*den,num};}
        append(start); const auto end=AudioTimeline::add(start,duration); if(end)append(*end);
    }
    return {{"context",m_audioEditContext},{"points",points}};
}

QVariantMap Backend::previewAudioGesture(const QVariantMap &input, const QString &action,
                                         const QVariantMap &inputChanges, int sampleRate) const {
    const auto target=plainVariant(input).toMap(), changes=plainVariant(inputChanges).toMap();
    auto error=audioEditProblem(target);
    const auto context=target.value("context").toMap();
    if (error.isEmpty() && sampleRate!=m_projectSettings.value("sampleRate",48000).toInt())
        error=tr("The project sample rate changed. Start the sound gesture again.");
    if (error.isEmpty() && (changes.contains("destinationTrack") || changes.value("ripple").toBool())
        && !AudioTimeline::sameEditContext(context,m_audioEditContext,true))
        error=tr("The track arrangement changed. Start the sound gesture again.");
    // Precise targets follow their identity when tracks are reordered. Only an
    // explicit destination belongs to the captured arrangement/revision.
    const auto current=target.value("clipId").toString().isEmpty()
        ? entryOn(target.value("track",-1).toInt(),target.value("index",-1).toInt())
        : AudioTimeline::entryWithId(m_connected,target.value("clipId").toString());
    const int destination=changes.value("destinationTrack",current.value("track",-1)).toInt();
    if (error.isEmpty() && (destination<1 || destination>=m_tracks.size()
        || m_tracks[destination].toMap().value("kind").toString()!="audio"
        || m_tracks[destination].toMap().value("locked").toBool()))
        error=tr("Choose an unlocked audio track for the sound edit.");
    if (error.isEmpty() && destination!=current.value("track").toInt() && action!="move")
        error=tr("Only a move can change the sound track.");
    if (!error.isEmpty()) return {{"valid",false},{"error",error}};
    auto preview=AudioTimingEdit::gesture(target.value("entry").toMap(),context.value("fpsNum").toInt(),
        context.value("fpsDen").toInt(),action,changes,sampleRate);
    if (preview.value("valid").toBool() && changes.contains("destinationTrack")) preview["changed"]=true;
    return preview;
}

int Backend::applyAudioGesture(const QVariantMap &input, const QString &action,
                                const QVariantMap &inputChanges, int sampleRate) {
    const int request=++m_audioEditRequest;
    const auto target=plainVariant(input).toMap(),changes=plainVariant(inputChanges).toMap();
    const auto preview=previewAudioGesture(target,action,changes,sampleRate);
    if (!preview.value("valid").toBool()) {
        const auto error=preview.value("error").toString();
        QTimer::singleShot(0,this,[this,request,error] {setStatus(error);emit audioGestureFinished(request,false,error,{});});
    } else emit audioGestureRequested(request,target,action,changes,sampleRate);
    return request;
}

QVariantMap Backend::retimeTarget(int track, int index) const {
    const auto entry=entryOn(track,index);
    if (entry.isEmpty()) return {};
    const auto sequence=m_sequences.value(m_activeSequence).toMap();
    return {{"track",track},{"index",index},{"entry",entry},{"entryState",MarkerEdit::clipState(entry)},
            {"generation",m_mediaGeneration},{"sequenceId",sequence.value("id")},{"sequenceName",sequence.value("name")},
            {"fps",m_fps>0 ? m_fps : 25.0}};
}
QString Backend::retimeProblem(const QVariantMap &input,bool segments) const {
    const auto target=plainVariant(input).toMap();
    if (target.isEmpty()) return tr("This clip is no longer available.");
    if (projectBusy() || m_exporting) return tr("Wait for the current project operation before changing speed.");
    if (target.value("generation").toInt()!=m_mediaGeneration ||
        target.value("sequenceId").toString()!=m_sequences.value(m_activeSequence).toMap().value("id").toString())
        return tr("The project or sequence changed. Close this dialog and reopen the clip.");
    const int track=target.value("track",-1).toInt(),index=target.value("index",-1).toInt();
    if (track<0 || track>=m_tracks.size() || m_tracks[track].toMap().value("locked").toBool())
        return tr("The clip's track is locked or no longer available.");
    const auto entry=entryOn(track,index);
    if (entry.isEmpty() || MarkerEdit::clipState(entry)!=target.value("entryState").toString())
        return tr("The clip changed. Close this dialog and reopen it to continue.");
    if(segments) {
        if(entry.value(ClipKey::gap).toBool() || entry.contains(ClipKey::compoundJson) || !entry.value(ClipKey::title).toString().isEmpty())
            return tr("Choose a media clip to edit its speed segments.");
        if(entry.value(ClipKey::offline).toBool())return tr("Relink this clip before changing its speed segments.");
        if(!entry.value(ClipKey::audioOnly).toBool() && !entry.value(ClipKey::link).toString().isEmpty())for(const auto &v:m_connected) {
            const auto mate=v.toMap();const int lane=mate.value(ClipKey::track,mate.value(ClipKey::lane)).toInt();
            if(mate.value(ClipKey::link)==entry.value(ClipKey::link) && mate.value(ClipKey::audioOnly).toBool()
                && lane>=0 && lane<m_tracks.size() && m_tracks[lane].toMap().value("locked").toBool())
                return tr("Unlock the linked audio track before changing speed.");
        }
        return {};
    }
    return RetimeEdit::preview(entry,entry.value(ClipKey::speed,1.0).toDouble()).value("error").toString();
}
QVariantMap Backend::previewRetime(const QVariantMap &input, double value, bool duration, bool reverse) const {
    const auto target=plainVariant(input).toMap();
    if (!std::isfinite(value) || value<=0 || (duration && (value>2147483646.0 || value!=std::floor(value))))
        return {{"valid",false},{"error",duration ? tr("Enter a duration in seconds, such as 2.5 or 1:30.") : tr("Enter a speed between 5% and 2000%.")}};
    const auto entry=target.value("entry").toMap();
    return duration ? RetimeEdit::forDuration(entry,int(value),reverse)
                    : RetimeEdit::preview(entry,(reverse ? -1.0 : 1.0)*value/100.0);
}
int Backend::applyRetime(const QVariantMap &input, double speed) {
    const int request=++m_retimeRequest;
    const auto target=plainVariant(input).toMap();
    auto error=retimeProblem(target);
    if (error.isEmpty()) error=RetimeEdit::preview(target.value("entry").toMap(),speed).value("error").toString();
    if (!error.isEmpty()) {
        QTimer::singleShot(0,this,[this,request,error]{setStatus(error);emit retimeFinished(request,false,error);});
    } else emit retimeRequested(request,target,speed);
    return request;
}

int Backend::prepareSpeedRamp(const QVariantMap &input) {
    const int request=++m_retimeRequest;const auto target=plainVariant(input).toMap();
    const auto error=retimeProblem(target,true);
    if(!error.isEmpty())QTimer::singleShot(0,this,[this,request,error]{emit speedRampPrepared(request,{{"valid",false},{"error",error}});});
    else emit speedRampPrepareRequested(request,target);
    return request;
}
QVariantMap Backend::editSpeedRampDraft(const QVariantMap &input,const QString &action,int index,const QVariantMap &options) const {
    return SpeedRampEdit::edit(plainVariant(input).toMap(),action,index,plainVariant(options).toMap());
}
QVariantMap Backend::previewSpeedRamp(const QVariantMap &input) const {return SpeedRampEdit::inspect(plainVariant(input).toMap());}
QVariantMap Backend::speedRampSourceMatch(const QVariantMap &input,int position) const {
    const auto entry=plainVariant(input).toMap();RetimeMap timing;QString error;
    if(!timing.set(entry.value(ClipKey::retime).toMap(),&error))return {{"valid",false},{"error",error}};
    QVariantMap metadata=entry.value(ClipKey::sourceMeta).toMap();double recordedFps=0;
    for(const auto &v:m_library)if(v.toMap().value("path")==entry.value(ClipKey::path)) {
        auto source=v.toMap();recordedFps=source.value("sourceFps").toDouble();
        if(recordedFps<=0)recordedFps=source.value("fps").toDouble();
        for(auto it=metadata.begin();it!=metadata.end();++it)source[it.key()]=it.value();metadata=source;break;
    }
    const double fps=m_fps>0?m_fps:25.;double sourceFps=metadata.value("sourceFps").toDouble();
    if(sourceFps<=0)sourceFps=recordedFps>0?recordedFps:metadata.value("fps").toDouble();
    if(sourceFps<=0)sourceFps=fps; // still images and older projects have no recorded rate
    const double factor=Timeline::interpretationFactor(metadata),scale=factor*sourceFps/fps;
    if(!std::isfinite(scale) || scale<=0 || timing.sourceLength()*scale>2147483646.)
        return {{"valid",false},{"error",tr("The speed ramp has an invalid source frame rate.")}};
    const int first=entry.value(ClipKey::mixIn).toInt(),count=entry.value(ClipKey::frames).toInt();
    const auto prepared=SpeedRampEdit::prepare(entry,timing.sourceLength());
    if(!prepared.value("valid").toBool())return prepared;
    RetimeMap cut,visible;
    if(!cut.set(prepared.value("definition").toMap(),&error) || !visible.set(cut.window(first,count,&error),&error))
        return {{"valid",false},{"error",error}};
    int offset=position-entry.value(ClipKey::start).toInt();if(offset<0 || offset>=count)offset=0;
    auto picture=[&](int at) {
        const int mode=entry.value(ClipKey::frameBlend).toInt();
        if(FrameSampling::explicitPictures(mode) || entry.contains(ClipKey::sourceOrigin))
            return qMin(visible.pictureSample(at,fps/factor,sourceFps,mode).first,
                qMax(0,int(std::ceil(timing.sourceLength()*scale-1e-9))-1));
        return qFuzzyCompare(factor,1.)?visible.sourceFrame(at,fps,sourceFps)
            :qBound(0,qRound(visible.sourceFrame(at,fps,fps)*scale),qMax(0,int(std::ceil(timing.sourceLength()*scale))-1));
    };
    const auto bounds=visible.sourceBounds();const int frame=picture(offset);
    int in=std::max(0,int(std::floor(bounds.first*scale+1e-7)));
    int out=std::max(in,int(std::ceil(bounds.second*scale-1e-7))-1);
    // Held reverse boundaries select the preceding picture; interpreted
    // sources may round either edge to a recorded frame.
    int start=0;for(const auto &v:visible.definition().value("segments").toList()) {
        const int frames=v.toMap().value("frames").toInt();
        for(int at:{start,start+frames-1}){const int p=picture(at);in=std::min(in,p);out=std::max(out,p);}start+=frames;
    }
    return {{"valid",true},{"frame",frame},{"in",in},{"out",out}};
}
int Backend::applySpeedRamp(const QVariantMap &input,const QVariantMap &definition,bool normal) {
    const int request=++m_retimeRequest;const auto target=plainVariant(input).toMap(),draft=plainVariant(definition).toMap();
    auto error=retimeProblem(target,true);if(error.isEmpty())error=SpeedRampEdit::inspect(draft).value("error").toString();
    if(!error.isEmpty())QTimer::singleShot(0,this,[this,request,error]{setStatus(error);emit retimeFinished(request,false,error);});
    else emit speedRampRequested(request,target,draft,normal);
    return request;
}

int Backend::quickRetime(int track, int index, const QString &kind, int clipFrame, double rate) {
    const QVariantMap target = retimeTarget(track, index);
    const QVariantMap entry = target.value(QStringLiteral("entry")).toMap();
    const int request = ++m_retimeRequest;
    const auto finish = [this, request](const QString &error) {
        QTimer::singleShot(0, this, [this, request, error] { setStatus(error); emit retimeFinished(request, false, error); });
    };
    if (!QStringList{"hold", "blade", "replay", "rewind"}.contains(kind) || !std::isfinite(rate) || rate <= 0 || rate > 20) {
        finish(tr("Choose a retime: hold, blade speed, instant replay or rewind."));
        return request;
    }
    const QString problem = retimeProblem(target, true);
    if (!problem.isEmpty()) { finish(problem); return request; }
    // The speed map counts from the clip's first picture under a dissolve.
    const int frame = clipFrame + entry.value(ClipKey::mixIn).toInt();
    const int preparing = prepareSpeedRamp(target);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &Backend::speedRampPrepared, this,
                          [this, connection, preparing, request, target, kind, frame, rate](int id, const QVariantMap &state) {
        if (id != preparing) return;
        disconnect(*connection);
        const auto fail = [this, request](const QString &error) { setStatus(error); emit retimeFinished(request, false, error); };
        if (!state.value(QStringLiteral("valid")).toBool()) { fail(state.value(QStringLiteral("error")).toString()); return; }
        QVariantMap result;
        const QVariantMap draft = state.value(QStringLiteral("definition")).toMap();
        if (kind == QLatin1String("hold")) {
            const double fps = target.value(QStringLiteral("fps")).toDouble() > 0 ? target.value(QStringLiteral("fps")).toDouble() : (m_fps > 0 ? m_fps : 25.);
            result = SpeedRampEdit::edit(draft, QStringLiteral("hold"), -1, {{QStringLiteral("frame"), frame}, {QStringLiteral("frames"), qMax(1, qRound(2 * fps))}});
        } else if (kind == QLatin1String("blade")) {
            result = SpeedRampEdit::edit(draft, QStringLiteral("split"), -1, {{QStringLiteral("frame"), frame}});
        } else {
            result = SpeedRampEdit::edit(draft, kind, -1, {});
            // Rewind runs back at the chosen speed: each reversed segment.
            if (kind == QLatin1String("rewind") && result.value(QStringLiteral("valid")).toBool() && !qFuzzyCompare(rate, 1.)) {
                const QVariantList rows = result.value(QStringLiteral("segments")).toList();
                for (int i = 0; i < rows.size() && result.value(QStringLiteral("valid")).toBool(); ++i) {
                    if (rows.at(i).toMap().value(QStringLiteral("fromRate")).toDouble() >= 0) continue;
                    const double speed = rows.at(i).toMap().value(QStringLiteral("fromRate")).toDouble() * rate;
                    result = SpeedRampEdit::edit(result.value(QStringLiteral("definition")).toMap(), QStringLiteral("set"), i,
                                                 {{QStringLiteral("speed"), speed}, {QStringLiteral("endSpeed"), speed}, {QStringLiteral("keepSource"), true}});
                }
            }
        }
        if (!result.value(QStringLiteral("valid")).toBool()) {
            fail(kind == QLatin1String("blade") || kind == QLatin1String("hold")
                     ? tr("Put the playhead inside the clip, away from its first frame.")
                     : result.value(QStringLiteral("error")).toString());
            return;
        }
        // Applied under this request's number, so callers see one answer.
        const int applying = applySpeedRamp(target, result.value(QStringLiteral("definition")).toMap());
        auto done = std::make_shared<QMetaObject::Connection>();
        *done = connect(this, &Backend::retimeFinished, this, [this, done, applying, request](int finished, bool changed, const QString &error) {
            if (finished != applying) return;
            disconnect(*done);
            emit retimeFinished(request, changed, error);
        });
    });
    return request;
}
