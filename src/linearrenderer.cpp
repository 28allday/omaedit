#include "linearrenderer.h"
#include "stagetimer.h"
#include "bands.h"
#include "imagebands.h"
#include "projectfile.h"
#include "solidcolour.h"
#include "retimemap.h"
#include "framesampling.h"
#include "sourcetiming.h"
#include "animatedwebp.h"
#include "motionflow.h"
#include "renderrequest.h"
#include "nativevideo.h"
#include "colourpipeline.h"
#include "gradeengine.h"
#include "editengine.h"
#include "timeline.h"
#include "rendersnapshot.h"
#include "projectsafety.h"
#include "framing.h"
#include <QScopedValueRollback>
#include <QJsonDocument>
#include <QFileInfo>
#include <QMutex>
#include <QTransform>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <cstring>
extern "C" {
#include <libswscale/swscale.h>
}

namespace {
int consumerRgbMaximum() {
    // Qualify the installed encoder converter's RGB16 numeric range. Some
    // libswscale builds use 255 << 8 as full white, shifting a conventional
    // 65535 input by three 10-bit legal-range codes. This is final encoder
    // quantization only; stored/output-transform float pixels remain intact.
    static const int maximum=[] {
        auto *context=sws_getContext(16,2,AV_PIX_FMT_RGBA64LE,16,2,AV_PIX_FMT_YUV444P16LE,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if(!context)return 0;
        const auto *coefficients=sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(context,coefficients,1,coefficients,0,0,1<<16,1<<16);
        quint16 rgb[16*2*4],planes[3][16*2];
        for(int i=0;i<16*2;++i){for(int c=0;c<3;++c)rgb[i*4+c]=32768;rgb[i*4+3]=65535;}
        const uint8_t *input[]={reinterpret_cast<uint8_t *>(rgb),nullptr,nullptr,nullptr};const int inStride[]={16*8,0,0,0};
        uint8_t *output[]={reinterpret_cast<uint8_t *>(planes[0]),reinterpret_cast<uint8_t *>(planes[1]),reinterpret_cast<uint8_t *>(planes[2]),nullptr};const int outStride[]={32,32,32,0};
        const int rows=sws_scale(context,input,inStride,0,2,output,outStride);sws_freeContext(context);
        if(rows!=2)return 0;
        if(std::abs(int(planes[0][8])-32128)<=1)return 65535;
        if(std::abs(int(planes[0][8])-32238)<=1)return 65280;
        return 0;
    }();
    return maximum;
}
QVariantMap managementFor(const QVariantMap &entry) {
    return ColourPipeline::effective(entry.value("grade").toMap(),!entry.value("title").toString().isEmpty());
}
bool hidden(const QVariantMap &e,const QVariantList &tracks) {
    const int t=e.value("track",e.value("lane",0)).toInt();
    // A sound generator (tone:) has no picture: nothing to draw.
    // Nor has a recording held on a picture track (the clip list marks it).
    return e.value("audioOnly").toBool() || e.value("disabled").toBool() || e.value("gap").toBool()
        || e.value("pictureless").toBool() || e.value("path").toString().startsWith("tone:")
        || (t>=0 && t<tracks.size() && (tracks[t].toMap().value("disabled").toBool() || tracks[t].toMap().value("kind")=="audio"));
}
double parameter(const QVariantMap &e,const char *name,int frame,double fallback=0) {
    return EditEngine::valueAt(e.value("editKeys").toMap().value(name).toList(),frame,e.value(name,fallback).toDouble());
}
void interpolated(const QImage &image,double x,double y,float *out) {
    const int ix=std::floor(x),iy=std::floor(y);double alpha=0,rgb[3]={};
    for(int j=0;j<2;++j)for(int i=0;i<2;++i) {
        if(ix+i<0 || iy+j<0 || ix+i>=image.width() || iy+j>=image.height())continue;
        const auto *p=reinterpret_cast<const float *>(image.constScanLine(iy+j))+(ix+i)*4;
        const double weight=(i?x-ix:1-x+ix)*(j?y-iy:1-y+iy)*p[3];
        alpha+=weight;for(int c=0;c<3;++c)rgb[c]+=weight*p[c];
    }
    for(int c=0;c<3;++c)out[c]=alpha>0?rgb[c]/alpha:0;
    out[3]=std::clamp(alpha,0.,1.);
}
QImage transformed(const QImage &source,const QSize &size,const QVariantMap &e,int frame,bool before,double sourceAspect=1,double targetAspect=1,render::Scaling scaling=render::Scaling::Fit) {
    const auto bypass=e.value("bypass").toStringList();
    QTransform transform;
    const double pixelRatio=sourceAspect/targetAspect;
    const auto fit=render::scaleInto(scaling,source.width(),source.height(),pixelRatio,size.width(),size.height());
    if(!before && !bypass.contains("rect")) {
        const double rw=parameter(e,"rectW",frame,1),rh=parameter(e,"rectH",frame,1);
        const double ax=parameter(e,"anchorX",frame,.5)*size.width(),ay=parameter(e,"anchorY",frame,.5)*size.height();
        transform.translate(parameter(e,"rectX",frame)*size.width()+rw*ax,parameter(e,"rectY",frame)*size.height()+rh*ay);
        transform.rotate(parameter(e,"rotation",frame));transform.scale(rw,rh);transform.translate(-ax,-ay);
    }
    transform.translate((size.width()-source.width()*fit.x)/2,(size.height()-source.height()*fit.y)/2);
    transform.scale(fit.x,fit.y);
    if(transform.isIdentity() && source.size()==size)return source;
    bool invertible=false;const auto inverse=transform.inverted(&invertible);
    QImage out(size,QImage::Format_RGBA32FPx4);out.fill(Qt::transparent);
    if(!invertible)return out;
    for(int y=0;y<out.height();++y) {
        auto *p=reinterpret_cast<float *>(out.scanLine(y));
        for(int x=0;x<out.width();++x,p+=4) {
            auto point=inverse.map(QPointF(x+.5,y+.5));double sx=point.x()-.5,sy=point.y()-.5;
            interpolated(source,sx,sy,p);
        }
    }
    return out;
}
// The CPU twin of the engine's framing (render/framing.h): `source` drawn
// into the box `p` of a picture of `size`, the part of it p's u and v name,
// bilinear; the rest transparent.
QImage placedOn(const QImage &source,const QSize &size,const render::Placement &p) {
    QImage out(size,QImage::Format_RGBA32FPx4);out.fill(Qt::transparent);
    if(source.isNull() || p.w<=0 || p.h<=0)return out;
    const double su=(p.u1-p.u0)*source.width()/p.w,sv=(p.v1-p.v0)*source.height()/p.h;
    const int x0=std::max(0,int(std::floor(p.x))),x1=std::min(size.width(),int(std::ceil(p.x+p.w)));
    const int y0=std::max(0,int(std::floor(p.y))),y1=std::min(size.height(),int(std::ceil(p.y+p.h)));
    uchar *const base=out.bits();const qsizetype stride=out.bytesPerLine();
    const int rows=Bands::rowsPerBand(size.width(),16),bands=(y1-y0+rows-1)/rows;
    Bands::run(std::max(0,bands),[&](int band){
        for(int y=y0+band*rows,end=std::min(y1,y0+(band+1)*rows);y<end;++y){
            auto *o=reinterpret_cast<float *>(base+y*stride)+x0*4;
            const double sy=p.v0*source.height()+(y+.5-p.y)*sv-.5;
            for(int x=x0;x<x1;++x,o+=4)interpolated(source,p.u0*source.width()+(x+.5-p.x)*su-.5,sy,o);
        }
        return true;});
    return out;
}
// A Gaussian over `image` in place, reaching `radius` pixels at a sigma of a
// third of it (the engine's Blur), across then down, edges held.
void blurred(QImage &image,float radius) {
    const int reach=int(std::ceil(radius-0.001f));if(reach<=0)return;
    const double sigma=radius/3.;std::vector<double> taps(size_t(reach)*2+1);double sum=0;
    for(int k=-reach;k<=reach;++k)sum+=taps[size_t(k+reach)]=std::exp(-k*k/(2*sigma*sigma));
    for(double &t:taps)t/=sum;
    const int w=image.width(),h=image.height();std::vector<float> line;
    for(int pass=0;pass<2;++pass) {
        const int n=pass?h:w,count=pass?w:h;line.resize(size_t(n)*4);
        for(int i=0;i<count;++i) {
            const auto at=[&](int j)->float *{return pass?reinterpret_cast<float *>(image.scanLine(j))+i*4:reinterpret_cast<float *>(image.scanLine(i))+j*4;};
            for(int j=0;j<n;++j)std::memcpy(&line[size_t(j)*4],at(j),16);
            for(int j=0;j<n;++j){double acc[4]={};
                for(int k=-reach;k<=reach;++k){const float *q=&line[size_t(std::clamp(j+k,0,n-1))*4];const double t=taps[size_t(k+reach)];for(int c=0;c<4;++c)acc[c]+=t*q[c];}
                float *o=at(j);for(int c=0;c<4;++c)o[c]=float(acc[c]);}
        }
    }
}
// The working picture made `size` by `framing`: fitted (transparent bars),
// filled (cropped), or fitted over its own copy filling a small frame,
// blurred, stretched over the frame and dimmed over black.
QImage framed(const QImage &source,const QSize &size,render::Framing framing,double pixelAspect) {
    const double aspect=source.height()>0?source.width()*pixelAspect/source.height():1.;
    QImage picture=placedOn(source,size,render::placeIn(aspect,size.width(),size.height(),framing==render::Framing::Fill));
    if(framing!=render::Framing::Blur)return picture;
    const QSize small(render::backdropSide(size.width()),render::backdropSide(size.height()));
    QImage back=placedOn(source,small,render::placeIn(aspect,small.width(),small.height(),true));
    for(int y=0;y<back.height();++y){auto *q=reinterpret_cast<float *>(back.scanLine(y));
        for(int x=0;x<back.width();++x,q+=4){const float a=std::clamp(q[3],0.f,1.f);for(int c=0;c<3;++c)q[c]*=a;q[3]=1;}}
    blurred(back,render::kBackdropRadius);
    render::Placement whole;whole.w=size.width();whole.h=size.height();
    const QImage stretched=placedOn(back,size,whole);
    const float dim=render::kBackdropOpacity;
    for(int y=0;y<size.height();++y){
        auto *o=reinterpret_cast<float *>(picture.scanLine(y));const auto *b=reinterpret_cast<const float *>(stretched.constScanLine(y));
        for(int x=0;x<size.width();++x,o+=4,b+=4){
            const float a=std::clamp(o[3],0.f,1.f),under=dim*std::clamp(b[3],0.f,1.f);
            for(int c=0;c<3;++c)o[c]=o[c]*a+b[c]*under*(1-a);
            o[3]=1;
        }
    }
    return picture;
}
}

struct LinearRenderer::State {
    QVariantList entries,tracks;
    QVariantMap project;
    QSize size;
    double fps=25;
    QHash<QString,std::shared_ptr<NativeVideo>> decoders;
    QHash<QString,std::shared_ptr<Timeline>> nests;
    static bool projectDocument(const QString &path) {
        return ProjectFile::isProject(path);
    }
    // An OmaEdit timeline (it carries its own snapshot), drawn as a nest.
    QHash<QString,std::pair<QDateTime,bool>> owned;
    bool ownTimeline(const QString &path) {
        const QDateTime modified=QFileInfo(path).lastModified();
        const auto it=owned.constFind(path);
        if(it!=owned.constEnd() && it->first==modified)return it->second;
        const bool own=!ProjectSafety::readDocument(path).isEmpty();
        owned.insert(path,{modified,own});
        return own;
    }
    QVector<QPair<QVariantMap,RetimeMap>> timingMaps;
    QMutex mutex;
    bool matteView=false;
    LinearRenderer::FrameSource source;

    // The delivery's picture at `frame`: the working frame (from the frame
    // source when there is one), at `size` when given, flattened over black
    // unless the delivery keeps transparency, through `space`, quantised to
    // 16 bits as the encoder's RGB16 path expects. Null with `error` set.
    QImage deliver(int frame,const QSize &size,const QString &space,QString *error,WorkingFrame *linear=nullptr) {
        *error=source?QString():check(entries,0,true);
        StageTimer::Sequence stages;stages.next("deliver.source");
        auto working=!error->isEmpty()?WorkingFrame{}:source?source(frame,error):sequence(entries,frame,false,error);
        stages.next("deliver.flatten");
        if(!error->isEmpty() || working.isNull()){if(error->isEmpty())*error="The frame could not be rendered.";return {};}
        // An export of another size: placed on its frame as the delivery's
        // framing asks (the engine's own framing, on the CPU).
        if(size.width()>0 && size.height()>0 && size!=working.pixels().size())
            working=working.withPixels(framed(working.pixels(),size,render::framingFrom(project.value("framing").toString()),project.value("pixelAspect",1.).toDouble()),error);
        // Without an alpha channel in the delivery, transparency is black
        // underneath (over black in linear light, then output-encoded).
        if(!project.value("transparent").toBool()) {
            // Only a picture with transparency in it is copied and flattened.
            const QImage &pixels=working.pixels();
            const int rows=Bands::rowsPerBand(pixels.width(),16),bands=(pixels.height()+rows-1)/rows;
            const auto opaque=[&](int band){
                for(int y=band*rows,end=qMin(pixels.height(),y+rows);y<end;++y){const auto *p=reinterpret_cast<const float *>(pixels.constScanLine(y));
                    for(int x=0;x<pixels.width();++x,p+=4)if(std::clamp(p[3],0.f,1.f)<1.f)return false;}
                return true;};
            if(!Bands::run(bands,opaque)) {
                QImage flat=pixels.copy();
                uchar *const base=flat.bits();const qsizetype stride=flat.bytesPerLine();
                Bands::run(bands,[&](int band){
                    for(int y=band*rows,end=qMin(flat.height(),y+rows);y<end;++y){auto *p=reinterpret_cast<float *>(base+y*stride);
                        for(int x=0;x<flat.width();++x,p+=4){const float a=std::clamp(p[3],0.f,1.f);if(a<1.f){for(int c=0;c<3;++c)p[c]*=a;p[3]=1.f;}}}
                    return true;});
                working=working.withPixels(flat,error);
            }
            if(!error->isEmpty() || working.isNull())return {};
        }
        if(linear){*linear=std::move(working);return {};}
        stages.next("deliver.output");
        const auto output=working.output(space,error);
        if(!error->isEmpty() || output.isNull())return {};
        stages.next("deliver.quantize");
        // Final quantization only. The encoder may convert this to its chosen
        // YUV bit depth; no integer video effect is evaluated.
        const int maximum=consumerRgbMaximum();
        if(!maximum){*error="The encoder's RGB16 range could not be qualified for linear output.";return {};}
        QImage quantized(output.size(),QImage::Format_RGBA64);
        uchar *const base=quantized.bits();const qsizetype stride=quantized.bytesPerLine();
        const int rows=Bands::rowsPerBand(output.width(),16),bands=(output.height()+rows-1)/rows;
        const bool done=Bands::run(bands,[&](int band){
            if(RenderScope::cancelled())return false;
            for(int y=band*rows,end=qMin(output.height(),y+rows);y<end;++y){
                const auto *p=reinterpret_cast<const float *>(output.constScanLine(y));auto *q=reinterpret_cast<quint16 *>(base+y*stride);
                for(int x=0;x<output.width();++x,p+=4,q+=4){for(int c=0;c<3;++c)q[c]=qRound(std::clamp(p[c],0.f,1.f)*maximum);q[3]=qRound(std::clamp(p[3],0.f,1.f)*65535);}
            }
            return true;});
        if(!done)return {};
        return quantized;
    }

    const RetimeMap *timingFor(const QVariantMap &entry,QString *error) {
        const auto definition=entry.value("retime").toMap();
        if(!entry.contains("retime"))return nullptr;
        if(definition.isEmpty() || !qFuzzyCompare(entry.value("speed",1.).toDouble(),1.)) {
            *error="The clip has an invalid speed ramp.";return nullptr;
        }
        for(const auto &cached:std::as_const(timingMaps))if(cached.first==definition)return &cached.second;
        RetimeMap map;if(!map.set(definition,error))return nullptr;
        timingMaps.append({definition,map});return &timingMaps.last().second;
    }

    // Constant-rate blending and optical flow still require the external
    // bake path. Mapped frame blending samples working pixels directly.
    QString check(const QVariantList &list,int depth=0,bool output=false) const {
        const double aspect=project.value("pixelAspect",1.).toDouble();
        if(!size.isValid() || !std::isfinite(fps) || fps<=0 || !std::isfinite(aspect) || aspect<1./32 || aspect>32)return "The linear render profile is invalid.";
        if(depth>8)return "Linear sequences are nested too deeply.";
        for(const auto &v:list) {
            const auto e=v.toMap();if(hidden(e,depth?QVariantList{}:tracks))continue;
            const QString name=e.value("name",e.value("path")).toString();
            const auto bypass=e.value("bypass").toStringList();
            for (const auto &v : e.value("videoEffects").toList())
                if (!v.toMap().value("bypass").toBool())
                    return name+": video effects are not supported in linear rendering. Bypass the effect or switch colour management off to continue.";
            if(!e.value("stab").toMap().isEmpty() && !bypass.contains("stab"))return name+": stabilisation is not supported in linear rendering. Bypass it to continue.";
            if(e.value("blend").toInt()!=0 && !bypass.contains("blend"))return name+": this blend mode is not supported in linear rendering.";
            if((e.value("lensK1").toDouble()!=0 || e.value("lensK2").toDouble()!=0) && !bypass.contains("lens"))return name+": lens correction is not supported in linear rendering. Bypass it to continue.";
            const int sampling=e.value("frameBlend").toInt();
            if(!FrameSampling::valid(sampling))return name+": invalid frame sampling mode.";
            if(output && FrameSampling::interpolates(sampling) && !e.contains("retime") && !e.contains("sourceOrigin"))
                return name+": this frame sampling mode can't be exported with colour management yet. Choose Floor or Nearest frame, or use Frame blending with speed segments.";
            const auto m=managementFor(e);
            const auto error=ColourPipeline::validate(m);if(!error.isEmpty())return name+": "+error;
            const auto grade=e.value("grade").toMap();
            if(!grade.isEmpty() && !bypass.contains("grade")) {const auto p=GradeEngine::validate(grade);if(!p.isEmpty())return name+": "+p;}
            const auto compound=e.value("compoundJson").toString();
            if(!compound.isEmpty()) {const auto p=check(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(),depth+1,output);if(!p.isEmpty())return p;}
            const auto style=e.value("mixStyle").toString();
            if(!style.isEmpty() && style!="dissolve" && style!="wipe-left" && style!="wipe-right")return name+": this transition style is not supported in linear rendering.";
        }
        return {};
    }
    WorkingFrame wrap(const QImage &pixels,QString *error) const {
        const auto info=ColourPipeline::info();
        return WorkingFrame::fromLinear(pixels,info.value("config").toString(),info.value("configHash").toString(),"oma_interchange",error);
    }
    WorkingFrame blendFrames(const WorkingFrame &first,const WorkingFrame &second,double mix,QString *error,int mode=1) const {
        if(first.isNull() || second.isNull())return {};
        auto image=first.pixels();const auto &other=second.pixels();
        if(image.size()!=other.size()){*error="The sampled source pictures have different dimensions.";return {};}
        if(mode==2) {
            const auto motion=MotionFlow::cached(image,other,error,RenderScope::cancelled);
            if(!motion)return {};
            const auto result=motion->interpolate(image,other,mix,error,RenderScope::cancelled);
            return result.isNull()?WorkingFrame():first.withPixels(result,error);
        }
        for(int y=0;y<image.height();++y) {
            auto *a=reinterpret_cast<float *>(image.scanLine(y));
            const auto *b=reinterpret_cast<const float *>(other.constScanLine(y));
            for(int x=0;x<image.width();++x,a+=4,b+=4) {
                const double wa=(1-mix)*a[3],wb=mix*b[3],alpha=wa+wb;
                for(int c=0;c<3;++c)a[c]=alpha>0?(a[c]*wa+b[c]*wb)/alpha:0;
                a[3]=std::clamp(alpha,0.,1.);
            }
        }
        return first.withPixels(image,error);
    }
    WorkingFrame effects(WorkingFrame input,const QVariantMap &e,int local,bool before,QString *error) {
        if(before)return input;
        auto bypass=e.value("bypass").toStringList();
        QStringList order=e.value("effectOrder").toStringList();
        for(const auto &key:QStringList{"flip","color","grade","crop","rect","opacity"})if(!order.contains(key))order<<key;
        for(const auto &key:order) {
            if(bypass.contains(key))continue;
            if(key=="flip" && (e.value("flipH").toBool() || e.value("flipV").toBool()))input=input.withPixels(input.pixels().flipped((e.value("flipH").toBool()?Qt::Horizontal:Qt::Orientations{}) | (e.value("flipV").toBool()?Qt::Vertical:Qt::Orientations{})),error);
            else if(key=="grade" && !e.value("grade").toMap().isEmpty()) input=GradeEngine::renderWorking(input,e.value("grade").toMap(),local,error,matteView);
            else if(key=="color" || key=="crop") {
                // Resolve animated values at this frame. A feather or inverted
                // crop can affect coverage even when all four edges are zero.
                if(key=="crop" && !e.value("cropInvert").toBool()
                    && parameter(e,"cropL",local)==0 && parameter(e,"cropR",local)==0
                    && parameter(e,"cropT",local)==0 && parameter(e,"cropB",local)==0
                    && parameter(e,"cropFeather",local)==0)continue;
                input=input.withPixels(EditEngine::render(input.pixels(),{{"group",key},{"base",e},{"keys",e.value("editKeys")}},local),error);
            } else if(key=="rect") input=input.withPixels(transformed(input.pixels(),size,e,local,false),error);
            else if(key=="opacity") {
                double alpha=e.value("opacity",1.).toDouble();
                alpha=std::clamp(EditEngine::valueAt(e.value("opacityKf").toList(),local,alpha,"alpha"),0.,1.);
                if(alpha==1)continue;
                auto image=input.pixels();
                for(int y=0;y<image.height();++y){auto *p=reinterpret_cast<float *>(image.scanLine(y));for(int x=0;x<image.width();++x)p[x*4+3]*=alpha;}
                input=input.withPixels(image,error);
            }
            if(input.isNull())return {};
        }
        auto image=input.pixels();const int fi=e.value("fadeIn").toInt(),fo=e.value("fadeOut").toInt();
        const int length=e.value("frames").toInt()+e.value("mixIn").toInt();
        double fade=1.;if(fi>0)fade=std::min(fade,double(local)/std::max(1,fi-1));
        if(fo>0)fade=std::min(fade,double(length-1-local)/std::max(1,fo-1));
        fade=std::clamp(fade,0.,1.);
        if(fade<1){for(int y=0;y<image.height();++y){auto *p=reinterpret_cast<float *>(image.scanLine(y));for(int x=0;x<image.width();++x)p[x*4+3]*=fade;}return input.withPixels(image,error);}
        return input;
    }
    WorkingFrame clip(const QVariantMap &e,int local,bool before,QString *error,int depth) {
        // An offline clip (its file is gone) shows a dark slate until relinked.
        const QString own=e.value("path").toString();
        const bool missing=e.value("offline").toBool()||(QFileInfo(own).isAbsolute()&&!QFileInfo::exists(own));
        const QString path=missing?QStringLiteral("color:#181820"):own;const auto m=managementFor(e);
        const double speed=e.value("speed",1.).toDouble();
        const int mode=e.value("frameBlend").toInt();
        const bool explicitPictures=FrameSampling::explicitPictures(mode);
        std::optional<double> origin;
        if(!SourceTiming::readOrigin(e,origin)){*error="The clip has an invalid source origin.";return {};}
        const bool nativePictures=explicitPictures || origin.has_value();
        const int engineMode=origin && !e.contains("retime") && mode==FrameSampling::Original ? FrameSampling::Nearest : mode;
        const int outputPosition=e.contains("freeze")?e.value("freeze").toInt():e.value("in").toInt()+local;
        double position=outputPosition;
        if(!nativePictures && !e.contains("freeze") && speed<0)position=e.value("length").toInt()-1-position;
        const RetimeMap *timing=timingFor(e,error);
        RetimeMap conform;
        if(!error->isEmpty())return {};
        const bool blend=timing && FrameSampling::interpolates(mode);
        if(timing) {
            const auto point=timing->sample(outputPosition);
            if(!point.valid){*error="The speed ramp frame is outside its timing map.";return {};}
            position=point.source;
        }
        WorkingFrame frame;
        const auto compound=e.value("compoundJson").toString();
        if(!compound.isEmpty())frame=sequence(QJsonDocument::fromJson(compound.toUtf8()).toVariant().toList(),qRound(position*std::abs(speed)),before,error,depth+1);
        else if(projectDocument(path) && ownTimeline(path)) {
            if(depth>8){*error="Linear nested sequence recursion limit exceeded.";return {};}
            if(!nests.contains(path)) {
                auto nested=std::make_shared<Timeline>();QStringList library;
                if(!nested->loadDocument(path,&library)){*error="Cannot open nested sequence: "+path;return {};}
                nests[path]=std::move(nested);
            }
            auto nested=nests.value(path);const auto problem=check(nested->entries(),depth+1);
            if(!problem.isEmpty()){*error=problem;return {};}
            const double factor=Timeline::interpretationFactor(e.value("sourceMeta").toMap());
            if(nativePictures && !timing) {
                if(!conform.set(RetimeMap::conform(nested->frameCount(),nested->fps(),fps,speed,factor,origin),error))return {};
                timing=&conform;
            }
            if(timing) {
                const auto sample=timing->pictureSample(outputPosition,nativePictures?fps/factor:fps,nested->fps(),engineMode);
                if(sample.first<0){*error="The speed ramp has an invalid source frame rate.";return {};}
                frame=nested->renderLinearAt(qMin(sample.first,nested->frameCount()-1),before,error);
                if(!frame.isNull() && sample.mix>0) {
                    const auto next=nested->renderLinearAt(qMin(sample.second,nested->frameCount()-1),before,error);
                    frame=blendFrames(frame,next,sample.mix,error,mode);
                }
            } else frame=nested->renderLinearAt(qRound(position*std::abs(speed)*nested->fps()/fps),before,error);
        } else {
            QImage image,otherImage;double mix=0;
            if(path.startsWith("color:") && e.value("title").toString().isEmpty()) {
                QColor color=solidColour(path.mid(6));if(!color.isValid()){*error="Invalid solid source colour.";return {};}
                image=QImage(size,QImage::Format_RGBA32FPx4);image.fill(color);
            } else if(!e.value("title").toString().isEmpty()) {
                image=QImage(size,QImage::Format_RGBA32FPx4);image.fill(Qt::transparent);
                QVariantMap style;for(const auto &key:QStringList{"Font","Color","Size","Place","Align","Weight","Italic","Bg","Outline","PixelSize","Tracking","LineSpacing","Margin","Width"}) {
                    QString name=key;name[0]=name[0].toLower();style[name]=e.value("title"+key);
                }
                image=EditEngine::render(image,{{"group","title"},{"style",style},{"text",e.value("title")},{"height",size.height()}},local);
            } else if(projectDocument(path)) {
                *error="Not an OmaEdit project: "+path;
                return {};
            } else {
                if(!decoders.contains(path)) {
                    if(decoders.size()>=12)decoders.clear();
                    auto decoder=std::make_shared<NativeVideo>();if(!decoder->open(path,error))return {};
                    decoders[path]=std::move(decoder);
                }
                const bool still=MediaPlayer::isStillPath(path);
                const auto facts=decoders[path]->information();
                const auto animation=nativePictures?AnimatedWebP::inspect(path):AnimatedWebP::Info{};
                const double sourceFps=animation.animated && animation.tick>0
                    ? 1000./animation.tick : facts.value("fps",fps).toDouble();
                const double factor=Timeline::interpretationFactor(e.value("sourceMeta").toMap());
                const int nativeFrames=animation.animated && animation.tick>0
                    ? int(animation.milliseconds/animation.tick) : facts.value("frameCount").toInt();
                if(nativePictures && !still && !timing) {
                    if(!conform.set(RetimeMap::conform(nativeFrames,sourceFps,fps,speed,factor,origin),error))return {};
                    timing=&conform;
                }
                // An interpreted rate plays the source at speed*factor, as the
                // timeline's retime does (50p read as 25p: half speed).
                double seconds=position*std::abs(speed)*factor/fps;
                // Played as recorded: the nearest source frame (as the engine picks it).
                if(!timing && !still && qFuzzyCompare(std::abs(speed),1.) && sourceFps>0)
                    seconds=std::floor(seconds*sourceFps+.5+1e-9)/sourceFps;
                if(timing) {
                    if(!std::isfinite(sourceFps) || sourceFps<=0 || !std::isfinite(factor) || factor<=0
                        || timing->sourceLength()*factor*sourceFps/fps>2147483646.) {
                        *error="The speed ramp has an invalid source frame rate.";return {};
                    }
                    const bool interpreted=!qFuzzyCompare(factor,1.0);
                    auto sample=nativePictures?timing->pictureSample(outputPosition,fps/factor,sourceFps,engineMode)
                        :blend?timing->imageSample(outputPosition,fps,interpreted?fps:sourceFps)
                        :RetimeMap::ImageSample{timing->sourceFrame(outputPosition,fps,interpreted?fps:sourceFps),-1,0};
                    if(sample.first<0){*error="The speed ramp has an invalid source frame rate.";return {};}
                    if(nativePictures) {
                        if(nativeFrames<1){*error="Cannot determine the source picture bounds.";return {};}
                        sample.first=qMin(sample.first,nativeFrames-1);
                        sample.second=qMin(sample.second,nativeFrames-1);
                    } else if(interpreted) {
                        // Preserve the two-stage selection of saved legacy maps.
                        const int last=qMax(0,int(std::ceil(timing->sourceLength()*factor*sourceFps/fps))-1);
                        sample.first=qBound(0,qRound(sample.first*factor*sourceFps/fps),last);
                        if(sample.mix>0)sample.second=qBound(0,qRound(sample.second*factor*sourceFps/fps),last);
                    }
                    seconds=sample.first/sourceFps;
                    if(!still && sample.mix>0) {
                        otherImage=decoders[path]->frame(sample.second/sourceFps,e.value("sourceMeta").toMap(),error);
                        if(otherImage.isNull())return {};
                        mix=sample.mix;
                    }
                }
                image=decoders[path]->frame(still?0:std::max(0.,seconds),e.value("sourceMeta").toMap(),error);
                if(image.isNull())return {};
            }
            const QString lut=e.value("sourceMeta").toMap().value("inputLut").toString();
            if(path.startsWith("color:") || !e.value("title").toString().isEmpty())
                image.setText("omaedit.pixelAspect",QString::number(project.value("pixelAspect",1.).toDouble(),'g',16));
            if(!lut.isEmpty())image=ColourPipeline::applyLut(image,lut,error);
            if(image.isNull())return {};
            frame=WorkingFrame::fromInput(image,m,error);
            if(!frame.isNull() && mix>0) {
                if(!lut.isEmpty())otherImage=ColourPipeline::applyLut(otherImage,lut,error);
                if(otherImage.isNull())return {};
                const auto next=WorkingFrame::fromInput(otherImage,m,error);
                frame=blendFrames(frame,next,mix,error,e.value("frameBlend").toInt());
            }
        }
        if(frame.isNull())return {};
        // Fit native dimensions in linear premultiplied space before clip effects.
        const double sourceAspect=frame.pixels().text("omaedit.pixelAspect").isEmpty()?1:frame.pixels().text("omaedit.pixelAspect").toDouble();
        const double targetAspect=project.value("pixelAspect",1.).toDouble();
        auto fitted=transformed(frame.pixels(),size,{},0,true,sourceAspect,targetAspect,
                                render::scalingFor(e.value("scaling").toString(),project.value("scaling").toString()));
        fitted.setText("omaedit.pixelAspect",QString::number(targetAspect,'g',16));
        frame=frame.withPixels(fitted,error);
        return effects(frame,e,local,before,error);
    }
    WorkingFrame sequence(const QVariantList &list,int position,bool before,QString *error,int depth=0) {
        if(RenderScope::cancelled())return {};
        if(depth>8){*error="Linear sequence recursion limit exceeded.";return {};}
        QImage canvas(size,QImage::Format_RGBA32FPx4);canvas.fill(Qt::transparent);
        canvas.setText("omaedit.pixelAspect",QString::number(project.value("pixelAspect",1.).toDouble(),'g',16));auto result=wrap(canvas,error);
        if(result.isNull())return {};
        QMap<int,QVariantList> active;
        for(const auto &v:list){const auto e=v.toMap();if(hidden(e,depth?QVariantList{}:tracks))continue;
            const int start=e.value("start").toInt()-e.value("mixIn").toInt();
            if(position>=start && position<e.value("start").toInt()+e.value("frames").toInt())active[e.value("track",e.value("lane",0)).toInt()]<<e;
        }
        for(const auto &layer:active) {
            WorkingFrame foreground;QVariantMap previous;
            for(const auto &v:layer) {
                const auto e=v.toMap();const int local=position-e.value("start").toInt()+e.value("mixIn").toInt();
                if(e.value("adjust").toBool()) {result=effects(result,e,local,before,error);if(result.isNull())return {};continue;}
                const auto next=clip(e,local,before,error,depth);if(next.isNull())return {};
                if(foreground.isNull())foreground=next;
                else {
                    const int overlap=e.value("mixIn").toInt();
                    if(overlap<=0){*error="Unexpected overlapping clips in the linear timeline.";return {};}
                    const double progress=std::clamp(double(local)/std::max(1,overlap-1),0.,1.);
                    const auto style=previous.value("mixStyle").toString();
                    // wipe-right: the edge travels right, the next clip in from the left.
                    if(style.startsWith("wipe-")) {
                        auto image=foreground.pixels();const auto &b=next.pixels();
                        for(int y=0;y<image.height();++y){auto *p=reinterpret_cast<float *>(image.scanLine(y));const auto *q=reinterpret_cast<const float *>(b.constScanLine(y));
                            for(int x=0;x<image.width();++x)if((style=="wipe-right"?double(x+.5)/image.width():1-double(x+.5)/image.width())<progress)std::copy(q+x*4,q+x*4+4,p+x*4);}
                        foreground=foreground.withPixels(image,error);
                    }else foreground=WorkingFrame::dissolve(foreground,next,progress,error);
                }
                previous=e;
            }
            if(!foreground.isNull())result=WorkingFrame::over(result,foreground,1,error);
            if(result.isNull())return {};
        }
        return result;
    }
};

LinearRenderer::LinearRenderer():d(std::make_shared<State>()){}
LinearRenderer::~LinearRenderer()=default;
bool LinearRenderer::enabled(const QVariantList &,const QVariantMap &,int) {
    // Every timeline is colour-managed (OpenColorIO, linear working light).
    return true;
}
void LinearRenderer::configure(const QVariantList &entries,const QVariantList &tracks,const QVariantMap &project,const QSize &size,double fps) {
    QMutexLocker lock(&d->mutex);
    if(d->entries!=entries || d->project!=project){d->nests.clear();d->timingMaps.clear();}
    d->entries=entries;d->tracks=tracks;d->project=project;d->size=size;d->fps=fps;
}
QString LinearRenderer::validate() const {QMutexLocker lock(&d->mutex);return d->source?QString():d->check(d->entries,0,true);}
void LinearRenderer::setFrameSource(FrameSource source) {QMutexLocker lock(&d->mutex);d->source=std::move(source);}
WorkingFrame LinearRenderer::render(int frame,bool before,QString *error) {
    RenderScope scope;
    QString local;if(!error)error=&local;
    static thread_local int depth=0;
    if(depth>=8){*error="Linear sequence recursion limit exceeded.";return {};}
    QScopedValueRollback<int> nesting(depth,depth+1);
    QMutexLocker lock(&d->mutex);
    *error=d->check(d->entries);if(!error->isEmpty())return {};
    auto result=d->sequence(d->entries,frame,before,error);
    if(RenderScope::cancelled()){error->clear();return {};}
    RenderScope::fail(*error);
    *error=RenderScope::current()->error();
    return error->isEmpty()?result:WorkingFrame{};
}
QString LinearRenderer::outputSpace(const QString &delivery,const QString &toneMap) {
    if(delivery=="linear")return "oma_interchange";
    const QString view=toneMap.isEmpty() || toneMap=="none"?QString():'@'+toneMap;
    if(delivery=="pq" || delivery=="hlg")return "rec2020/"+delivery+view;
    if(delivery=="srgb")return "srgb/srgb"+view;
    if(delivery=="rec601")return "rec601/rec709"+view;
    if(delivery=="rec2020")return "rec2020/rec709"+view;
    return "rec709/rec709"+view;
}
QImage LinearRenderer::matte(int frame,QString *error) {
    QString local;if(!error)error=&local;
    QMutexLocker lock(&d->mutex);*error=d->check(d->entries);if(!error->isEmpty())return {};
    QScopedValueRollback<bool> view(d->matteView,true);
    // Matte pixels are a diagnostic surface, without working-image tags.
    return d->sequence(d->entries,frame,false,error).pixels();
}
QImage LinearRenderer::deliver(int frame,const QSize &size,const QString &space,QString *error) {
    QString local;if(!error)error=&local;
    QMutexLocker lock(&d->mutex);
    return d->deliver(frame,size,space,error);
}
WorkingFrame LinearRenderer::deliverLinear(int frame,const QSize &size,QString *error) {
    QString local;if(!error)error=&local;
    QMutexLocker lock(&d->mutex);
    WorkingFrame linear;
    d->deliver(frame,size,{},error,&linear);
    if(linear.isNull() && error->isEmpty())*error="The frame could not be rendered.";
    return linear;
}
