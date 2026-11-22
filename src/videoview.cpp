#include "videoview.h"

#include <QPainter>
#include <QQuickWindow>
#include <QJsonDocument>
#include <cmath>
#include "playbackview.h"

#include "backend.h"
#include "gradeengine.h"
#include "colourpipeline.h"
#include "viewerprocessor.h"

VideoView::VideoView(QQuickItem *parent) : SuiteQuick::ImageSurface(parent) {
    setClip(true);
    m_retry.setSingleShot(true);m_retry.setInterval(50);
    connect(&m_retry,&QTimer::timeout,this,&VideoView::refresh);
    connect(this,&QQuickItem::visibleChanged,this,[this]{invalidate();refresh();});
    connect(this,&QQuickItem::widthChanged,this,&VideoView::viewGeometryChanged);
    connect(this,&QQuickItem::heightChanged,this,&VideoView::viewGeometryChanged);
    connect(this,&VideoView::viewGeometryChanged,this,&VideoView::syncSurface);
    // A newer processed result can arrive before the GUI receives the older
    // frame's render acknowledgement. The older frame was still displayed.
    connect(this,&SuiteQuick::ImageSurface::imageRendered,this,[this](quint64 serial){if(m_backend && m_acknowledgeFrame)m_backend->framePresented(serial);});
}

VideoView::~VideoView(){if(m_processor)m_processor->cancel(m_client);}
void VideoView::setProcessing(bool value){if(m_processing!=value){m_processing=value;emit processingChanged();}}
void VideoView::invalidate(){
    ++m_epoch;m_requestKey.clear();m_contextKey.clear();m_retry.stop();
    if(m_processor)m_processor->cancel(m_client);
    setProcessing(false);m_frame={};m_acknowledgeFrame=false;syncSurface();update();
}

void VideoView::paint(QPainter *painter) {
    painter->fillRect(boundingRect(), canvasColor());
    if (m_frame.isNull())
        return;
    painter->fillRect(contentRect().intersected(boundingRect()), Qt::black);

    paintChecker(painter);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(contentRect(), m_frame);
    paintGuides(painter);
}

void VideoView::paintChecker(QPainter *painter) const {
    const QRectF target=contentRect();
    if(m_viewSettings.value("checker").toBool()) {
        painter->save(); painter->setClipRect(target.intersected(boundingRect()));
        const int tile=12;
        for(int y=0;y<height();y+=tile) for(int x=0;x<width();x+=tile)
            painter->fillRect(QRect(x,y,tile,tile),(x/tile+y/tile)%2 ? m_overlayPalette.value("checkerDark",QColor(Qt::darkGray)).value<QColor>() : m_overlayPalette.value("checkerLight",QColor(Qt::gray)).value<QColor>());
        painter->restore();
    }
}

void VideoView::paintGuides(QPainter *painter) const {
    const QRectF target=contentRect();
    if(m_overlaysEnabled && m_channel!=Still && m_channel!=Before && m_channel!=TwoUpOutgoing && m_channel!=TwoUpIncoming) {
        painter->save(); painter->setClipRect(boundingRect());
        QPen pen(m_overlayPalette.value("guides",QColor(Qt::white)).value<QColor>()); pen.setWidthF(1); pen.setStyle(Qt::DashLine); painter->setPen(pen);
        if(m_viewSettings.value("safeAreas").toBool()) for(double inset:{.05,.1}) painter->drawRect(target.adjusted(target.width()*inset,target.height()*inset,-target.width()*inset,-target.height()*inset));
        const QString guides=m_viewSettings.value("guides").toString();
        const int divisions=guides=="thirds" ? 3 : guides=="grid" ? 8 : guides=="cross" ? 2 : 0;
        for(int i=1;i<divisions;++i) {
            const double x=target.x()+target.width()*i/divisions,y=target.y()+target.height()*i/divisions;
            painter->drawLine(QPointF(x,target.top()),QPointF(x,target.bottom()));
            painter->drawLine(QPointF(target.left(),y),QPointF(target.right(),y));
        }
        painter->restore();
    }
}

QColor VideoView::canvasColor() const {
    const QColor canvas(m_viewSettings.value("canvas").toString());
    return canvas.isValid() ? canvas : QColor(PlaybackView::defaultCanvas);
}

void VideoView::syncSurface() {
    setImage(m_frame,m_acknowledgeFrame?m_serial:0);setImageRect(contentRect());
    // The canvas surrounds the picture; inside it, uncovered parts are black.
    setBackdrop(canvasColor(),Qt::black);
    const auto target=contentRect();
    const auto ratio=window()?window()->effectiveDevicePixelRatio():1.;
    const auto geometry=QString("%1,%2,%3,%4,%5,%6,%7,%8,%9").arg(width()).arg(height()).arg(target.x()).arg(target.y()).arg(target.width()).arg(target.height()).arg(ratio).arg(m_overlaysEnabled).arg(int(m_channel)).toUtf8();
    const auto key=geometry+QJsonDocument::fromVariant(m_viewSettings).toJson(QJsonDocument::Compact)+QJsonDocument::fromVariant(m_overlayPalette).toJson(QJsonDocument::Compact)+QByteArray::number(m_frame.isNull());
    if(key==m_layersKey)return;
    m_layersKey=key;
    if(m_frame.isNull() || width()<=0 || height()<=0){setLayers({},{});return;}
    // Decorative layers change only with view geometry/settings, never merely
    // because another video frame arrived. Bound their physical texture extent.
    const auto scale=qMin(ratio,4096./qMax(width(),height()));
    const QSize pixels(qMax(1,int(std::ceil(width()*scale))),qMax(1,int(std::ceil(height()*scale))));
    QImage checker,guides;
    if(m_viewSettings.value("checker").toBool()){checker=QImage(pixels,QImage::Format_ARGB32_Premultiplied);checker.fill(Qt::transparent);QPainter painter(&checker);painter.scale(scale,scale);paintChecker(&painter);}
    if(m_overlaysEnabled && (m_viewSettings.value("safeAreas").toBool() || m_viewSettings.value("guides").toString()!="none")){guides=QImage(pixels,QImage::Format_ARGB32_Premultiplied);guides.fill(Qt::transparent);QPainter painter(&guides);painter.scale(scale,scale);paintGuides(&painter);}
    setLayers(checker,guides);
}

void VideoView::itemChange(ItemChange change, const ItemChangeData &data) {
    SuiteQuick::ImageSurface::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange) {
        emit viewGeometryChanged();
        update();
    }
}

QRectF VideoView::contentRect() const {
    return PlaybackView::imageRect(size(),m_nativeSize,m_pixelAspect,m_viewTransformEnabled ? m_viewSettings : QVariantMap{},window() ? window()->effectiveDevicePixelRatio() : 1.);
}

QObject *VideoView::source() const {
    return m_backend.data();
}

void VideoView::setSource(QObject *source) {
    Backend *backend = qobject_cast<Backend *>(source);
    if (backend == m_backend)
        return;
    if (m_backend) {
        disconnect(m_backend->stills(), nullptr, this, nullptr);
        disconnect(m_backend, nullptr, this, nullptr);
    }
    invalidate();if(m_processor)disconnect(m_processor,nullptr,this,nullptr);
    m_backend = backend;m_processor=nullptr;m_client=0;
    if(m_backend){m_processor=m_backend->viewerProcessor();m_client=m_processor->registerClient();
        connect(m_processor,&ViewerProcessor::preferenceChanged,this,[this]{invalidate();refresh();});
    }
    if (m_backend) {
        connect(m_backend, &Backend::viewerSettingsChanged, this, &VideoView::refresh);
        connect(m_backend, &Backend::playingChanged, this, &VideoView::refresh); // paused: full resolution again
        connect(m_backend->stills(), &StillsGallery::itemsChanged, this, [this]{++m_stillRevision;if(m_channel==Still){invalidate();refresh();}});
        connect(m_backend, &Backend::frameChanged, this, [this]{if(m_channel==Program || m_channel==Auto)refresh();});
        connect(m_backend, &Backend::colorMatteChanged, this, [this]{if(m_channel==ColorMatte)refresh();});
        connect(m_backend, &Backend::effectPreviewChanged, this, [this]{if(m_channel==EffectPreview)refresh();});
        connect(m_backend, &Backend::sourceFrameChanged, this, [this]{if(m_channel==Source || (m_channel==Auto && m_backend->viewingSource()))refresh();});
        connect(m_backend, &Backend::beforeFrameChanged, this, [this]{if(m_channel==Before)refresh();});
        connect(m_backend, &Backend::twoUpFramesChanged, this, [this]{if(m_channel==TwoUpOutgoing || m_channel==TwoUpIncoming)refresh();});
        // Auto must repaint when the active view flips even if no new frame
        // has landed yet.
        connect(m_backend, &Backend::sourceChanged, this, [this]{if(m_channel==Auto || m_channel==Source){invalidate();refresh();}});
        connect(m_backend, &Backend::projectPlaybackReset, this, [this]{invalidate();refresh();});
        connect(m_backend, &QObject::destroyed, this, [this]{invalidate();m_backend=nullptr;m_processor=nullptr;m_client=0;});
        connect(m_backend, &Backend::gradeClipChanged, this, &VideoView::refresh);
    }
    emit sourceChanged();
    refresh();
}

void VideoView::setChannel(Channel channel) {
    if (channel == m_channel)
        return;
    invalidate();m_channel = channel;
    emit channelChanged();
    refresh();
}

void VideoView::setStillId(const QString &id) {if(m_stillId==id)return;invalidate();m_stillId=id;emit stillIdChanged();refresh();}

void VideoView::setColorOptions(const QVariantMap &options) {
    if (m_colorOptions == options) return;
    invalidate();m_colorOptions = options; emit colorOptionsChanged(); refresh();
}

namespace {
// The size a picture stands for: a timeline picture drawn small for the
// viewer says its whole size.
QSize fullSize(const QImage &frame) {
    const QStringList whole = frame.text(QStringLiteral("omaedit.fullSize")).split('x');
    if (whole.size() == 2 && whole[0].toInt() > 0 && whole[1].toInt() > 0) return {whole[0].toInt(), whole[1].toInt()};
    return frame.size();
}
}  // namespace

void VideoView::refresh() {
    if(!isVisible()){invalidate();return;}
    QImage frame;QString stillFile;QSize stillSize;
    if (!m_backend) frame={};
    else if (m_channel == Still) {
        const auto item=m_backend->stills()->item(m_stillId);stillFile=item.value("path").toString();stillSize=QSize(item.value("width").toInt(),item.value("height").toInt());
    }
    else if (m_channel == ColorMatte) frame=m_backend->colorMatteFrame();
    else if (m_channel == EffectPreview) frame=m_backend->effectPreviewFrame();
    else if (m_channel == Program) frame=m_backend->programFrame();
    else if (m_channel == Source) frame=m_backend->sourceFrame();
    else if (m_channel == Before) frame=m_backend->beforeFrame();
    else if (m_channel == TwoUpOutgoing) frame=m_backend->twoUpOutgoing();
    else if (m_channel == TwoUpIncoming) frame=m_backend->twoUpIncoming();
    else frame=m_backend->currentFrame();
    if(stillFile.isEmpty()) {
        m_nativeSize=fullSize(frame);m_pixelAspect=frame.text("omaedit.pixelAspect").isEmpty()?1:frame.text("omaedit.pixelAspect").toDouble();
    } else if(m_requestKey.isEmpty()) {m_nativeSize=stillSize;m_pixelAspect=1;}
    m_viewSettings=m_backend ? m_backend->viewerSettings() : PlaybackView::normalized({});
    emit viewGeometryChanged();update();
    // The program viewer tells playback the size it shows a picture at, in
    // the picture's own pixels, so the timeline is drawn no bigger.
    if (m_backend && stillFile.isEmpty() && !m_nativeSize.isEmpty()
        && (m_channel == Program || (m_channel == Auto && !m_backend->viewingSource()))) {
        const QRectF shown = contentRect();
        const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.;
        const QSize pixels(int(std::ceil(shown.width() * dpr / std::max(1e-3, m_pixelAspect))) + 1 & ~1,
                           int(std::ceil(shown.height() * dpr)) + 1 & ~1);
        QSize draw = pixels.width() < m_nativeSize.width() && pixels.height() < m_nativeSize.height() ? pixels : QSize();
        // Playback resolution (half, quarter): playing, the timeline is drawn
        // at that part of its size where the viewer would have asked for more.
        // The picture keeps its place and size in the viewer.
        const QSize part = PlaybackView::reduced(m_nativeSize, m_viewSettings.value("previewDivisor",1).toInt());
        if (part != m_nativeSize && (draw.isEmpty() || part.width() < draw.width())) draw = part;
        m_backend->setViewerDrawSize(draw);
    }
    // The source viewer tells it the same of its clip's pictures.
    const bool showsSource = m_backend && stillFile.isEmpty() && (m_channel == Source || (m_channel == Auto && m_backend->viewingSource()));
    if (showsSource && !m_nativeSize.isEmpty()) {
        const QRectF shown = contentRect();
        const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.;
        const QSize pixels(int(std::ceil(shown.width() * dpr / std::max(1e-3, m_pixelAspect))) + 1 & ~1,
                           int(std::ceil(shown.height() * dpr)) + 1 & ~1);
        QSize draw = pixels.width() < m_nativeSize.width() && pixels.height() < m_nativeSize.height() ? pixels : QSize();
        const QSize part = PlaybackView::reduced(m_nativeSize, m_viewSettings.value("previewDivisor",1).toInt());
        if (part != m_nativeSize && (draw.isEmpty() || part.width() < draw.width())) draw = part;
        m_backend->setSourceViewerDrawSize(draw);
    }
    if((frame.isNull() && stillFile.isEmpty()) || !m_processor) {
        invalidate();
        if(!m_colorError.isEmpty()){m_colorError.clear();emit colorErrorChanged();}
        return;
    }
    QVariantMap options = m_colorOptions;
    // Other workspaces use the same standard view for OCIO-managed program
    // frames. A raw source viewer has its own media interpretation; never
    // borrow the selected timeline clip's encoding for an unrelated source.
    if (options.isEmpty() && m_backend && (m_channel == Program || (m_channel == Auto && !m_backend->viewingSource()))) {
        const auto clip = m_backend->gradeClip();
        const auto grade = clip.value("grade").toMap();
        const auto management = grade.value("management").toMap();
        if (management.value("engine") == "ocio" && management.value("enabled").toBool()) {
            const bool bypass = grade.value("bypass").toBool() || clip.value("bypass").toBool();
            const QString stage = bypass ? "input" : "output";
            options = {{"engine", "ocio"}, {"from", management.value(stage, "rec709")},
                       {"fromGamma", management.value(stage + "Gamma", "rec709")}};
        }
    }
    // Playback may make the program's picture for this display on the engine
    // (nothing then converts it here); a single channel, or colour management
    // switched off for the viewer, is made from the working frame as before.
    if (m_backend && stillFile.isEmpty() && (m_channel == Program || (m_channel == Auto && !m_backend->viewingSource()))) {
        const bool plain = options.value("enabled", true).toBool() && options.value("channel", "rgb").toString() == "rgb";
        m_backend->setViewerDisplay(plain ? QVariant(QVariantMap{{"gamut", options.value("gamut", "srgb")},
                                                                 {"gamma", options.value("gamma", "srgb")},
                                                                 {"view", options.value("view", "Standard")}})
                                          : QVariant());
    }
    if (showsSource) {
        const bool plain = options.value("enabled", true).toBool() && options.value("channel", "rgb").toString() == "rgb";
        m_backend->setSourceViewerDisplay(plain ? QVariant(QVariantMap{{"gamut", options.value("gamut", "srgb")},
                                                                       {"gamma", options.value("gamma", "srgb")},
                                                                       {"view", options.value("view", "Standard")}})
                                                : QVariant());
    }
    ViewerProcessing::Request request;
    request.frame=std::move(frame);request.file=stillFile;request.fileRevision=m_channel==Still?m_stillRevision:0;request.options=options;request.matte=m_channel==ColorMatte;
    // The playback resolution is for playing: paused, the picture is whole again.
    request.divisor=(m_channel==Still || m_channel==ColorMatte || !m_backend->playing()) ? 1 : m_viewSettings.value("previewDivisor",1).toInt();
    // The picture on screen stands until the next is ready, unless what is
    // shown changes kind. The size is the one a picture stands for: playback
    // moves between pictures drawn small and whole ones (a cached stretch, a
    // pause), and blanking the viewer between them showed as a flash.
    auto contextRequest=request;contextRequest.frame={};
    const auto context=ViewerProcessing::key(contextRequest)+'|'+QByteArray::number(m_channel)+'|'+m_stillId.toUtf8()+'|'+QByteArray::number(m_backend->viewingSource())+'|'+QByteArray::number(stillFile.isEmpty()?m_nativeSize.width():stillSize.width())+'x'+QByteArray::number(stillFile.isEmpty()?m_nativeSize.height():stillSize.height())+'|'+QByteArray::number(stillFile.isEmpty()?m_pixelAspect:1,'g',17);
    if(m_contextKey!=context){invalidate();m_contextKey=context;}
    const auto key=ViewerProcessing::key(request);
    if(m_requestKey==key)return; // Pan/guides/geometry never reconvert pixels.
    const auto epoch=m_epoch,number=++m_request,serial=m_backend->presentationSerial();
    const bool acknowledge=m_channel==Auto || (m_channel==Program && !m_backend->viewingSource()) || (m_channel==Source && m_backend->viewingSource());
    const bool accepted=m_processor->submit(m_client,this,request,[this,epoch,number,serial,acknowledge,key](const ViewerProcessing::Result &result){
        if(epoch!=m_epoch || number<m_accepted || !isVisible())return;
        m_accepted=number;m_frame=result.frame;m_nativeSize=result.nativeSize;m_pixelAspect=result.pixelAspect;m_serial=serial;m_acknowledgeFrame=acknowledge;emit viewGeometryChanged();
        // A completed frame remains useful while a newer one waits. Requiring
        // the newest serial here would starve playback when processing lags.
        if(m_requestKey==key)setProcessing(false);
        if(result.error!=m_colorError){m_colorError=result.error;emit colorErrorChanged();if(!m_colorError.isEmpty() && m_backend)m_backend->announce(m_colorError);}
        update();emit framePrepared();
    });
    if(accepted){m_requestKey=key;setProcessing(true);m_retry.stop();}
    else if(!m_retry.isActive())m_retry.start();
}
