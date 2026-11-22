#include "timeline_p.h"
#include "scopeprocessor.h"
#include "scopes.h"

bool Timeline::linearRendering() const {
    return LinearRenderer::enabled(entries(),m_project);
}
QVariantMap Timeline::linearSourceManagement(const QString &path) const {
    if(!linearRendering())return {};
    for(const auto &v:entries()){const auto e=v.toMap();if(e.value(ClipKey::path)==path){const auto m=e.value(ClipKey::grade).toMap().value("management").toMap();if(!m.isEmpty())return m;}}
    auto m=ColourPipeline::defaults();m["enabled"]=true;return m;
}
WorkingFrame Timeline::renderLinearAt(int position,bool before,QString *error) {
    if(!m_profile){if(error)*error=QCoreApplication::translate("Timeline", "The timeline is empty.");return {};}
    if(!m_linear)m_linear=std::make_unique<LinearRenderer>();
    auto project=m_project;project["pixelAspect"]=double(m_profile->sample_aspect_num())/m_profile->sample_aspect_den();
    m_linear->configure(entries(),tracks(),project,QSize(m_profile->width(),m_profile->height()),fps());
    return m_linear->render(position,before,error);
}
void Timeline::setEngineOnCpu(bool cpu) {
    if(m_engineOnCpu==cpu)return;
    m_engineOnCpu=cpu;m_renderer.reset();m_engine.reset();
}
QString Timeline::engineBackend() const {
    if(!m_engine)return {};
    return m_engine->deviceName()=="CPU"?QStringLiteral("cpu"):QStringLiteral("vulkan");
}
bool Timeline::configureEngineRenderer() {
    if(!m_profile)return false;
    // An engine that lost its GPU never draws again: dropped with its
    // renderer, and another made (render::engineLost says on what).
    if(render::engineLost(m_engine.get())){m_renderer.reset();m_engine.reset();}
    if(!m_engine)m_engine=m_engineOnCpu?engine::createCpuEngine():render::createEngine();
    if(!m_renderer){
        m_renderer=std::make_unique<render::TimelineRenderer>(*m_engine);
        if(m_decoderSources>0)m_renderer->setDecoderLimits(m_decoderSources,m_decoderBytes,m_decoderThreads,m_decoderPerFile);
    }
    auto project=m_project;project["pixelAspect"]=double(m_profile->sample_aspect_num())/m_profile->sample_aspect_den();
    // Pictures from proxies where the playback choice says so (an export
    // always reads its originals); a proxy keeps its original's encoding.
    if(m_proxyMode==QStringLiteral("original"))m_renderer->setPictureSource({});
    else m_renderer->setPictureSource([sources=m_mediaSources,mode=m_proxyMode](const QString &path) {
        const QVariantMap settings=sources.value(path).toMap();
        const QString proxy=settings.value(QStringLiteral("proxyPath")).toString();
        const bool wanted=mode==QStringLiteral("proxy") || (mode==QStringLiteral("preferred")
            && (settings.value(QStringLiteral("preferProxy"),true).toBool() || !QFileInfo::exists(path)));
        return wanted && !proxy.isEmpty() && QFileInfo::exists(proxy) ? proxy : path;
    });
    m_renderer->setMediaSources(m_mediaSources);
    m_renderer->setAside(m_aside);
    const QSize full(m_profile->width(),m_profile->height());
    const bool smaller=m_drawSize.width()>0 && m_drawSize.height()>0 && m_drawSize.width()<full.width() && m_drawSize.height()<full.height();
    // Effects measured in pixels keep their look at the smaller size.
    if(smaller){project["drawScale"]=double(m_drawSize.height())/full.height();project["fullSize"]=full;}
    m_renderer->configure(entries(),tracks(),project,smaller?m_drawSize:full,fps());
    return true;
}
int Timeline::clipsDrawn() const { return m_renderer ? m_renderer->stats().clips : 0; }
int Timeline::picturesReduced() const { return m_renderer ? m_renderer->stats().reduced : 0; }
int Timeline::picturesMadeSmall() const { return m_renderer ? m_renderer->stats().small : 0; }
int Timeline::picturesFetched() const { return m_renderer ? m_renderer->stats().fetched : 0; }
int Timeline::picturesFetchedAhead() const { return m_renderer ? m_renderer->stats().ahead : 0; }
void Timeline::setDecoderLimits(int sources, qint64 bytes, int threads, int perFile) {
    m_decoderSources = sources; m_decoderBytes = bytes; m_decoderThreads = threads; m_decoderPerFile = perFile;
    if (m_renderer) m_renderer->setDecoderLimits(sources, bytes, threads, perFile);
}
int Timeline::openSources(const QString &path) const { return m_renderer ? m_renderer->openSources(path) : 0; }
bool Timeline::engineRendering() {
    if(!render::engineRenderingRequested())return false;
    const bool drawn=configureEngineRenderer() && m_renderer->unsupported().isEmpty();
    // OMAEDIT_RENDERER_TRACE=1: why the engine is not drawing a timeline.
    static const bool trace=qEnvironmentVariableIsSet("OMAEDIT_RENDERER_TRACE");
    if(trace && !drawn && m_renderer)qInfo("engine renderer declined: %s",qPrintable(m_renderer->unsupported()));
    return drawn;
}
QString Timeline::engineUnsupported() {
    if(!configureEngineRenderer())return QCoreApplication::translate("Timeline", "The timeline is empty.");
    return m_renderer->unsupported();
}
QImage Timeline::renderEngineAt(int position,QString *error) {
    if(!configureEngineRenderer()){if(error)*error=QCoreApplication::translate("Timeline", "The timeline is empty.");return {};}
    QImage image=m_renderer->image(position,error);
    // Lost on this very frame: drawn again on the engine that replaces it.
    if(image.isNull() && m_engine->lost() && configureEngineRenderer()){if(error)error->clear();image=m_renderer->image(position,error);}
    static const bool trace=qEnvironmentVariableIsSet("OMAEDIT_RENDERER_TRACE");
    if(trace)qInfo("engine frame %d on %s: %s",position,m_engine->deviceName().c_str(),image.isNull()?qPrintable(error?*error:QString()):"drawn");
    return image;
}
bool Timeline::cachePlanes(int position, int layout, engine::YuvFrame &out, QString *error) {
    if (!engineRendering() || !m_renderer->managed()) return false;
    auto program = ColourPipeline::engineCacheProgram(true, error);
    if (!program) return false;
    engine::OutputColour colour;
    colour.format = engine::ColourFormat(layout);
    colour.fullRange = true;
    colour.space = engine::ColourSpace::Bt709;
    if (m_renderer->deliver(qBound(0, position, frameCount() - 1), program, colour, out, error)) return true;
    // Lost on this very frame: drawn again on the engine that replaces it.
    if (!m_engine->lost() || !engineRendering() || !m_renderer->managed()) return false;
    if (error) error->clear();
    return m_renderer->deliver(qBound(0, position, frameCount() - 1), std::move(program), colour, out, error);
}

QImage Timeline::deliveredPicture(int frame, QString *error) {
    if (!m_linear || m_deliverySpace.isEmpty()) return {};
    return m_linear->deliver(frame, QSize(width(), height()), m_deliverySpace, error);
}

QString Timeline::prepareLinearOutput(const QString &space) {
    if(!linearRendering()) {
        if(space!="rec709/rec709" && space!="rec601/rec709")
            return QCoreApplication::translate("Timeline", "Enable OpenColorIO colour conversion in the Colour workspace before exporting this colour space, or choose Rec.709 / Rec.601.");
        return {};
    }
    if(!m_linear)m_linear=std::make_unique<LinearRenderer>();
    auto project=m_project;project["pixelAspect"]=double(m_profile->sample_aspect_num())/m_profile->sample_aspect_den();
    // An export with an alpha channel keeps the frames' transparency; any
    // other is flattened over black by the output stage.
    project["transparent"]=m_exportAlpha;
    project["framing"]=m_exportFraming;
    m_linear->configure(entries(),tracks(),project,QSize(m_profile->width(),m_profile->height()),fps());
    // The engine draws the export's frames when it can draw the timeline: an
    // engine of its own (the export runs on the consumer's thread), on the
    // program's GPU device.
    m_linear->setFrameSource({});
    m_deliverPlanes={};
    if(render::engineRenderingRequested()) {
        // The export's engine and renderer, made from what they draw so that
        // they can be made again: an engine that loses its GPU part-way is
        // replaced and the frame drawn again, rather than the export failing.
        struct Drawn {
            std::unique_ptr<engine::Engine> engine; std::unique_ptr<render::TimelineRenderer> renderer;
            QVariantList entries,tracks; QVariantMap project,sources; QSize size,frameSize; double fps=0; QString framing;
            void make() {
                renderer.reset();engine.reset();
                engine=render::createEngine();
                renderer=std::make_unique<render::TimelineRenderer>(*engine);
                renderer->setMediaSources(sources);
                renderer->configure(entries,tracks,project,size,fps);
                renderer->setFraming(frameSize,render::framingFrom(framing));
            }
            // True when the engine was lost and another now stands in its place.
            bool replaced() { if(!render::engineLost(engine.get()))return false; make(); return renderer->unsupported().isEmpty(); }
        };
        auto drawn=std::make_shared<Drawn>();
        drawn->entries=entries();drawn->tracks=tracks();drawn->project=project;drawn->sources=m_mediaSources;
        drawn->size=QSize(m_profile->width(),m_profile->height());drawn->frameSize=m_exportFrameSize;drawn->fps=fps();drawn->framing=m_exportFraming;
        drawn->make();
        if(drawn->renderer->unsupported().isEmpty()) {
            m_linear->setFrameSource([drawn](int frame,QString *error){
                WorkingFrame made=drawn->renderer->working(frame,error);
                if(made.isNull() && drawn->replaced()){if(error)error->clear();made=drawn->renderer->working(frame,error);}
                return made;});
            if(drawn->renderer->managed())
                m_deliverPlanes=[drawn,space](int frame,const engine::OutputColour &colour,engine::YuvFrame &out,QString *error){
                    if(drawn->renderer->deliver(frame,space,colour,out,error))return true;
                    if(!drawn->replaced() || !drawn->renderer->managed())return false;
                    if(error)error->clear();
                    return drawn->renderer->deliver(frame,space,colour,out,error);};
        }
    }
    const auto error=m_linear->validate();if(!error.isEmpty())return error;
    m_deliverySpace=space;
    return {};
}


MediaPlayer::Decoded Timeline::decodeAt(int position, bool withAudio,
                                         int frequency, int channels, bool withImage) {
    RenderScope render;
    AudioEngine::ReadScope soundRead;
    if (withAudio) clearAudioMeters();
    const auto decode=[&]() -> MediaPlayer::Decoded {
    if (!primary())
        return MediaPlayer::Decoded();
    if (withImage && engineRendering()) {
        MediaPlayer::Decoded out;
        if(m_renderer->managed() && m_shownAs.isValid()) {
            // For a viewer and nothing else: the display made on the engine.
            const QVariantMap display=m_shownAs.toMap();
            QString why;
            auto program=ColourPipeline::engineProgram(QStringLiteral("oma_interchange"),
                ColourPipeline::spaceName(display.value(QStringLiteral("gamut"),QStringLiteral("srgb")).toString(),
                                          display.value(QStringLiteral("gamma"),QStringLiteral("srgb")).toString()),
                display.value(QStringLiteral("view"),QStringLiteral("Standard")).toString(),&why);
            if(program && m_scopesAs.isValid()) {
                // And its scopes counted beside it, through the project's output transform.
                const QVariantMap options=m_scopesAs.toMap();
                const QString space=options.value(QStringLiteral("outputSpace")).toString();
                render::TimelineRenderer::ScopeCount scopes;
                scopes.layout=Scopes::layout(options,200);
                if(space!=QLatin1String("oma_interchange"))
                    scopes.program=ColourPipeline::engineProgram(QStringLiteral("oma_interchange"),space,{},&why);
                if(scopes.program || space==QLatin1String("oma_interchange"))
                    out.image=m_renderer->shown(qBound(0,position,frameCount()-1),std::move(program),
                                                QSize(m_profile->width(),m_profile->height()),&why,&scopes);
                if(!out.image.isNull())
                    ScopeProcessing::attach(out.image,{options,std::make_shared<const std::vector<quint32>>(std::move(scopes.cells))});
            } else if(program)out.image=m_renderer->shown(qBound(0,position,frameCount()-1),std::move(program),
                                                   QSize(m_profile->width(),m_profile->height()),&why);
        }
        if(!out.image.isNull()) {
            // Shown: there is no working frame to go with it.
        } else if(m_renderer->managed()) {
            // Colour-managed: the working frame, as the colour-managed renderer gives it.
            out.working=m_renderer->working(qBound(0,position,frameCount()-1),&out.error);
            out.image=out.working.taggedImage();
            // Drawn small for a viewer: what it stands for.
            if(!out.image.isNull() && out.image.width()!=m_profile->width())
                out.image.setText(QStringLiteral("omaedit.fullSize"),QStringLiteral("%1x%2").arg(m_profile->width()).arg(m_profile->height()));
        } else {
            out.image=renderEngineAt(qBound(0,position,frameCount()-1),&out.error);
        }
        if(withAudio)out.audio=audioAt(position,frequency,channels);
        return out;
    }
    if (linearRendering()) {
        MediaPlayer::Decoded out;
        if(withImage){
            out.working=renderLinearAt(qBound(0,position,frameCount()-1),false,&out.error);
            out.image=out.working.taggedImage();
        }
        if(withAudio)out.audio=audioAt(position,frequency,channels);
        return out;
    }
    return MediaPlayer::Decoded();
    };
    auto out=decode();
    RenderScope::fail(out.error);
    out.cancelled=RenderScope::cancelled();
    out.error=RenderScope::current()->error();
    if(out.cancelled || !out.error.isEmpty()){out.image={};out.working={};out.audio.clear();if(withAudio)clearAudioMeters();}
    if(out.cancelled)out.error.clear();
    return out;
}

MediaPlayer::Decoded Timeline::decodeBefore(int position) {
    if (!primary())
        return MediaPlayer::Decoded();
    if(engineRendering()) {
        MediaPlayer::Decoded out;
        out.working=m_renderer->working(qBound(0,position,frameCount()-1),&out.error,render::TimelineRenderer::Mode::Before);
        out.image=out.working.taggedImage();return out;
    }
    if(linearRendering()) {
        MediaPlayer::Decoded out;
        out.working=renderLinearAt(position,true,&out.error);
        out.image=out.working.taggedImage();return out;
    }
    return MediaPlayer::Decoded();
}
