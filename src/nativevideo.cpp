#include "nativevideo.h"
#include "nativepixels_p.h"
#include "rawpixels.h"
#include "renderrequest.h"
#include <QtEndian>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/display.h>
}

namespace {
double sample(const AVFrame *frame,const AVPixFmtDescriptor *desc,int component,int x,int y) {
    const auto &c=desc->comp[component];
    if (desc->flags & AV_PIX_FMT_FLAG_FLOAT) {
        const uchar *p=frame->data[c.plane]+y*frame->linesize[c.plane]+x*c.step+c.offset;
        quint32 bits=(desc->flags & AV_PIX_FMT_FLAG_BE) ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
        float value; std::memcpy(&value,&bits,4); return value;
    }
    const bool byte=c.depth+c.shift<=8,bigEndian=desc->flags & AV_PIX_FMT_FLAG_BE;
    // FFmpeg's sub-byte big-endian components address the low byte of a
    // 16-bit word; their descriptor offset can be -1 (for example RGB565BE).
    const int offset=c.offset+(byte && bigEndian);
    const uchar *p=frame->data[c.plane]+y*frame->linesize[c.plane]+x*c.step+offset;
    const quint32 bits=byte ? *p : bigEndian ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
    return (bits>>c.shift)&((1u<<c.depth)-1);
}

struct ChromaAxis {
    int first, second;
    double firstWeight, secondWeight;
};
ChromaAxis chromaAxis(int position, double offset, int subsampling, int count) {
    const double coordinate=(position-offset)/subsampling;
    const int index=std::floor(coordinate);
    return {std::clamp(index,0,count-1),std::clamp(index+1,0,count-1),
            1-coordinate+index,coordinate-index};
}
}

namespace NativeVideoPixels {
QImage pixels(const AVFrame *frame,const QVariantMap &meta,QString *error) {
    const auto *desc=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_BAYER)) return RawPixels::develop(frame,meta,error);
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL|AV_PIX_FMT_FLAG_PAL|AV_PIX_FMT_FLAG_BITSTREAM|AV_PIX_FMT_FLAG_BAYER))
        || (desc->nb_components!=1 && desc->nb_components!=2 && desc->nb_components!=3 && desc->nb_components!=4)) {
        *error="The source pixel format is not supported by the linear decoder."; return {};
    }
    const bool floating=desc->flags & AV_PIX_FMT_FLAG_FLOAT, rgb=desc->flags & AV_PIX_FMT_FLAG_RGB;
    for (int c=0;c<desc->nb_components;++c) {
        if ((floating && desc->comp[c].depth!=32) || (!floating && (desc->comp[c].depth>16 || desc->comp[c].shift+desc->comp[c].depth>16))) {
            *error="The source component precision is not supported by the linear decoder."; return {};
        }
    }
    if (frame->flags & AV_FRAME_FLAG_INTERLACED) { *error="Deinterlace this source before enabling linear rendering."; return {}; }
    if (frame->width<=0 || frame->height<=0 || qint64(frame->width)*frame->height>64000000) { *error="Invalid or oversized decoded frame."; return {}; }
    QImage image(frame->width,frame->height,QImage::Format_RGBA32FPx4);
    if (image.isNull()) { *error="Cannot allocate decoded frame."; return {}; }
    int matrix=frame->colorspace;
    const int inputSpace=meta.value("inputSpace").toInt();
    if(inputSpace==601)matrix=AVCOL_SPC_SMPTE170M;
    if(inputSpace==709)matrix=AVCOL_SPC_BT709;
    if(inputSpace==2020)matrix=AVCOL_SPC_BT2020_NCL;
    const QString override=meta.value("colorSpace").toString().toLower();
    if (override.contains("2020")) matrix=AVCOL_SPC_BT2020_NCL;
    else if (override.contains("709")) matrix=AVCOL_SPC_BT709;
    else if (override.contains("601")) matrix=AVCOL_SPC_SMPTE170M;
    double kr=.2126,kb=.0722;
    if (!rgb && desc->nb_components>=3) {
        if (matrix==AVCOL_SPC_BT2020_NCL) {kr=.2627;kb=.0593;}
        else if (matrix==AVCOL_SPC_SMPTE170M || matrix==AVCOL_SPC_BT470BG || matrix==AVCOL_SPC_FCC) {kr=.299;kb=.114;}
        else if (matrix!=AVCOL_SPC_BT709 && matrix!=AVCOL_SPC_UNSPECIFIED) { *error=QString("Unsupported YUV matrix %1 for linear source format %2.").arg(matrix).arg(desc->name); return {}; }
        // Untagged: by its size, the one rule every reader shares.
        if (matrix==AVCOL_SPC_UNSPECIFIED && SuiteVideo::untaggedIsBt601(frame->height)) {kr=.299;kb=.114;}
    }
    bool full=frame->color_range==AVCOL_RANGE_JPEG || rgb || floating;
    const auto range=meta.value("dataLevels",meta.value("range")).toString();
    if (range=="full" || range=="pc") full=true;
    if (range=="video" || range=="limited" || range=="tv") full=false;
    double maximum[4]{},scale[4]{},chromaDivisor[4]{};
    for(int c=0;c<desc->nb_components;++c) {
        maximum[c]=floating?1.0:double((1u<<desc->comp[c].depth)-1);
        scale[c]=std::ldexp(1.,desc->comp[c].depth-8);
        chromaDivisor[c]=full?maximum[c]:224*scale[c];
    }
    const int sx=1<<desc->log2_chroma_w,sy=1<<desc->log2_chroma_h;
    const int cw=(frame->width+sx-1)/sx,ch=(frame->height+sy-1)/sy;
    // Untagged video chroma is MPEG's, level with the left luma sample (JPEG's
    // "J" formats put it midway): as the engine's sources read it.
    const bool jpegLayout=desc->name && std::strncmp(desc->name,"yuvj",4)==0;
    const bool chromaLeft=frame->chroma_location==AVCHROMA_LOC_LEFT || frame->chroma_location==AVCHROMA_LOC_TOPLEFT
        || frame->chroma_location==AVCHROMA_LOC_BOTTOMLEFT || (frame->chroma_location==AVCHROMA_LOC_UNSPECIFIED && !jpegLayout);
    const double ox=chromaLeft?0:(sx-1)*.5;
    const double oy=(frame->chroma_location==AVCHROMA_LOC_TOPLEFT || frame->chroma_location==AVCHROMA_LOC_TOP)?0:(sy-1)*.5;
    // U and V share their geometry. Compute horizontal positions once per frame
    // and vertical positions once per row, including the clamped odd-size edges.
    const bool yuv=!rgb && desc->nb_components>=3;
    std::vector<ChromaAxis> horizontal(yuv?frame->width:0);
    for(size_t x=0;x<horizontal.size();++x)horizontal[x]=chromaAxis(x,ox,sx,cw);
    auto normalized=[&](int c,int x,int y) {return sample(frame,desc,c,x,y)/maximum[c];};
    auto chroma=[&](int c,const ChromaAxis &x,const ChromaAxis &y) {
        // Preserve the reference's four-term accumulation and rounding order.
        double result=0;
        result+=sample(frame,desc,c,x.first,y.first)*x.firstWeight*y.firstWeight;
        result+=sample(frame,desc,c,x.second,y.first)*x.secondWeight*y.firstWeight;
        result+=sample(frame,desc,c,x.first,y.second)*x.firstWeight*y.secondWeight;
        result+=sample(frame,desc,c,x.second,y.second)*x.secondWeight*y.secondWeight;
        return (result-128*scale[c])/chromaDivisor[c];
    };
    for(int y=0;y<image.height();++y) {
        auto *out=reinterpret_cast<float *>(image.scanLine(y));
        const auto vertical=yuv?chromaAxis(y,oy,sy,ch):ChromaAxis{};
        for(int x=0;x<image.width();++x,out+=4) {
            if(rgb) for(int c=0;c<3;++c) out[c]=normalized(c,x,y);
            else {
                const double l=full ? normalized(0,x,y) : (sample(frame,desc,0,x,y)-16*scale[0])/(219*scale[0]);
                if(desc->nb_components<3) out[0]=out[1]=out[2]=l;
                else {
                    const double u=chroma(1,horizontal[x],vertical),v=chroma(2,horizontal[x],vertical);
                    out[0]=l+2*(1-kr)*v;out[2]=l+2*(1-kb)*u;
                    out[1]=(l-kr*out[0]-kb*out[2])/(1-kr-kb);
                }
            }
            out[3]=(desc->flags & AV_PIX_FMT_FLAG_ALPHA)?normalized(desc->nb_components-1,x,y):1;
        }
    }
    return image;
}
}

struct NativeVideo::State {
    SuiteVideo::Decoder decoder;
    double last=-1;
    QImage cached;
    QVariantMap cachedMeta;
    bool ready=false;
};
NativeVideo::NativeVideo():d(std::make_unique<State>()) {}
NativeVideo::~NativeVideo()=default;
bool NativeVideo::open(const QString &path,QString *error,const SuiteVideo::DecodeOptions &options) {
    QString local;if(!error)error=&local;error->clear();d=std::make_unique<State>();
    // A long way to a picture stops when a newer request has taken over (and
    // carries on if this one is asked for again), as the engine's sources do.
    auto asked=options;
    if(!asked.cancelled)asked.cancelled=[]{return RenderScope::cancelled();};
    if(!d->decoder.open(path,asked,error))return false;
    if(!d->decoder.information().value("displayMatrixSupported").toBool()) {
        // (Quarter turns and mirroring come upright from the decoder; this is a slant or a scale.)
        *error="A source shown at a slant is not yet supported in linear rendering.";return false;
    }
    d->ready=true;return true;
}
QImage NativeVideo::frame(double seconds,const QVariantMap &meta,QString *error) {
    QString local;if(!error)error=&local;error->clear();
    if(!d->ready || !std::isfinite(seconds) || seconds<0){*error="Invalid native source frame request.";return {};}
    if(seconds==d->last && meta==d->cachedMeta)return d->cached;
    const auto *frame=d->decoder.frame(seconds,error);
    if(!frame) {
        if(d->decoder.information().value("codec").toString()=="prores_raw")
            *error=QString("FFmpeg cannot decode this ProRes RAW frame; the camera's sensor layout may be unsupported. %1").arg(*error);
        return {};
    }
    d->cached=NativeVideoPixels::pixels(frame,meta,error);
    const auto sar=frame->sample_aspect_ratio;const double aspect=sar.num>0 && sar.den>0?av_q2d(sar):1.;
    if(!d->cached.isNull() && aspect!=1)d->cached.setText("omaedit.pixelAspect",QString::number(aspect,'g',16));
    if(!d->cached.isNull()){d->cachedMeta=meta;d->last=seconds;}
    return d->cached;
}
QVariantMap NativeVideo::information() const {return d->decoder.information();}
