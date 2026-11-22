#include "viewerprocessing.h"
#include "playbackview.h"
#include "workingframe.h"
#include "colourpipeline.h"
#include "gradeengine.h"
#include "imagebands.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QImageReader>
namespace ViewerProcessing {
QByteArray key(const Request &request) {
    return QByteArray::number(request.frame.cacheKey())+'|'+request.file.toUtf8()+'|'+QByteArray::number(request.fileRevision)+'|'+QByteArray::number(request.divisor)+'|'+QByteArray::number(request.matte)+'|'+QJsonDocument::fromVariant(request.options).toJson(QJsonDocument::Compact);
}
Result render(const Request &request,const std::atomic<bool> &cancelled,const ColourTransform &transform) {
    Result result;QElapsedTimer timer;timer.start();
    auto original=request.frame;
    if(!request.file.isEmpty()) {
        if(request.file.endsWith(".exr",Qt::CaseInsensitive))original=WorkingFrame::load(request.file,nullptr,&result.error).taggedImage();
        else {QImageReader reader(request.file);original=reader.read();if(original.isNull())result.error=reader.errorString();}
        if(original.isNull())return result;
    }
    // A timeline picture drawn small for the viewer stands for its whole size.
    const QStringList whole=original.text(QStringLiteral("omaedit.fullSize")).split('x');
    result.nativeSize=whole.size()==2 && whole[0].toInt()>0 && whole[1].toInt()>0 ? QSize(whole[0].toInt(),whole[1].toInt()) : original.size();
    const auto aspect=original.text("omaedit.pixelAspect");result.pixelAspect=aspect.isEmpty()?1:aspect.toDouble();
    // The playback resolution: a whole picture is reduced here; one playback
    // already drew that small (or smaller, for a small viewer) is left alone.
    const bool small=original.width()<=PlaybackView::reduced(result.nativeSize,request.divisor).width();
    auto frame=PlaybackView::preview(original,small?1:request.divisor);auto options=request.options;
    if(cancelled)return result;
    // Playback's picture, already as this display shows it (see
    // Timeline::setShownAs): nothing is left to do.
    if(!frame.text(QStringLiteral("omaedit.shown")).isEmpty() && frame.format()==QImage::Format_ARGB32_Premultiplied) {
        result.conversion={{"backend","engine"},{"reason","Displayed by the render engine."}};
        result.frame=std::move(frame);result.milliseconds=timer.nsecsElapsed()/1000000.;return result;
    }
    // Whole-picture views pack straight to the display format inside the
    // colour transform; a single-channel view still edits the float result.
    const bool packed=request.matte || options.value("channel","rgb").toString()=="rgb";
    const auto target=packed ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGBA32FPx4;
    if(WorkingFrame::isTagged(frame)) {
        const auto working=WorkingFrame::fromTagged(frame,&result.error);
        if(!working.isNull()) {
            if(!options.value("enabled",true).toBool())frame=working.output("oma_interchange",&result.error);
            else if(transform && !request.matte)frame=transform(working.pixels(),options,cancelled,&result.conversion,&result.error);
            else frame=working.display(options,&result.error,target);
        } else frame={};
        options["enabled"]=false; // Frame identity already supplied its input space.
    }
    if(cancelled)return result;
    if(!frame.isNull() && !options.isEmpty() && !request.matte) {
        if(options.value("enabled",true).toBool()) {
            if(options.value("engine")=="ocio")frame=ColourPipeline::display(frame,options,&result.error,target);
            else frame=GradeEngine::convert(frame,options.value("from","rec709").toString(),options.value("fromGamma","rec709").toString(),options.value("gamut","srgb").toString(),options.value("gamma","srgb").toString());
        }
        const QString channel=options.value("channel","rgb").toString();
        if(channel!="rgb") {
            frame=frame.convertToFormat(QImage::Format_RGBA8888);
            for(int y=0;y<frame.height();++y) {
                if(cancelled)return result;
                auto *p=frame.scanLine(y);
                for(int x=0;x<frame.width();++x,p+=4) {
                    const auto value=channel=="red" ? p[0] : channel=="green" ? p[1] : channel=="blue" ? p[2] : qGray(p[0],p[1],p[2]);
                    p[0]=p[1]=p[2]=value;
                }
            }
        }
    }
    if(cancelled)return result;
    // Do the float/integer display packing here as well; the scene graph paint
    // only needs to scale/composite a ready, owning display image.
    if(result.conversion.isEmpty())result.conversion={{"backend","cpu"},{"reason","CPU display path."}};
    result.frame=Bands::convertToFormat(frame,QImage::Format_ARGB32_Premultiplied);
    result.milliseconds=timer.nsecsElapsed()/1000000.;return result;
}
}
