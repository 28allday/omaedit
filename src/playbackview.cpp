#include "playbackview.h"
#include <cmath>
#include <algorithm>
#include <type_traits>
#include <QColor>
#include <QColorSpace>
#include "bands.h"
#include "imagebands.h"

QVariantMap PlaybackView::normalized(const QVariantMap &s) {
    auto finite=[&](const char *key,double fallback,double low,double high) {
        bool ok=false; const double value=s.value(key,fallback).toDouble(&ok);
        return ok && std::isfinite(value) ? std::clamp(value,low,high) : fallback;
    };
    QString mode=s.value("zoomMode","fit").toString();
    if (!QStringList{"fit","fill","custom"}.contains(mode)) mode="fit";
    QString guides=s.value("guides","none").toString();
    if (!QStringList{"none","thirds","cross","grid"}.contains(guides)) guides="none";
    int divisor=s.value("previewDivisor",1).toInt();
    if (divisor!=1 && divisor!=2 && divisor!=4) divisor=1;
    // The canvas is the viewer area around the picture. Opaque only, so the
    // picture's edges always show against it.
    QColor canvas(s.value("canvas").toString());
    if (!canvas.isValid()) canvas=QColor(defaultCanvas);
    canvas.setAlpha(255);
    return {{"canvas",canvas.name(QColor::HexRgb)},{"zoomMode",mode},{"zoomPercent",finite("zoomPercent",100,1,800)},
        {"panX",finite("panX",0,-1,1)},{"panY",finite("panY",0,-1,1)},
        {"previewDivisor",divisor},{"safeAreas",s.value("safeAreas",false).toBool()},
        {"guides",guides},{"checker",s.value("checker",false).toBool()}};
}
QSize PlaybackView::reduced(const QSize &whole,int divisor) {
    if ((divisor!=2 && divisor!=4) || whole.isEmpty()) return whole;
    return {qMax(2,(whole.width()/divisor+1)&~1),qMax(2,(whole.height()/divisor+1)&~1)};
}
QRectF PlaybackView::imageRect(const QSizeF &view,const QSize &native,double par,const QVariantMap &settings,double dpr) {
    if (view.isEmpty() || native.isEmpty()) return {};
    if (!std::isfinite(par) || par<=0) par=1;
    if (!std::isfinite(dpr) || dpr<=0) dpr=1;
    const auto s=normalized(settings);
    QSizeF size(native.width()*par/dpr,native.height()/dpr);
    if(s.value("zoomMode")=="custom") size*=s.value("zoomPercent").toDouble()/100.;
    else size.scale(view,s.value("zoomMode")=="fill" ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
    return {(view.width()-size.width())/2 + s.value("panX").toDouble()*qMax(0.,size.width()-view.width())/2,
            (view.height()-size.height())/2 + s.value("panY").toDouble()*qMax(0.,size.height()-view.height())/2,size.width(),size.height()};
}
namespace {
// One output pixel is the area average of its divisor x divisor block of the
// source (the last row/column of blocks is clipped to the image). Straight
// alpha is weighted by coverage, so transparent pixels lend no colour; a
// uniform picture stays exactly the same value. Integers round to nearest.
template<typename T>
void boxRows(const QImage &in,QImage &out,int divisor,int y0,int y1,bool straight,double full) {
    const int width=in.width(),height=in.height(),outWidth=out.width();
    for(int y=y0;y<y1;++y) {
        const int top=y*divisor,bottom=qMin(height,top+divisor);
        T *dst=reinterpret_cast<T *>(out.scanLine(y));
        for(int x=0;x<outWidth;++x) {
            const int left=x*divisor,right=qMin(width,left+divisor);
            const int count=(bottom-top)*(right-left);
            T *o=dst+x*4;
            if constexpr(!std::is_floating_point_v<T>) {
                // Integer sums; a block with full coverage (all opaque video)
                // needs no weighting and gives the identical rounded mean.
                qint64 sum[4]={0,0,0,0};
                for(int r=top;r<bottom;++r) {
                    const T *p=reinterpret_cast<const T *>(in.constScanLine(r))+left*4;
                    for(int c=left;c<right;++c,p+=4){sum[0]+=p[0];sum[1]+=p[1];sum[2]+=p[2];sum[3]+=p[3];}
                }
                const qint64 opaque=qint64(full)*count;
                if(!straight || sum[3]==opaque) {
                    for(int k=0;k<4;++k)o[k]=T((2*sum[k]+count)/(2*qint64(count)));
                    continue;
                }
            }
            double colour[3]={0,0,0},weight=0,alpha=0;
            for(int r=top;r<bottom;++r) {
                const T *p=reinterpret_cast<const T *>(in.constScanLine(r))+left*4;
                for(int c=left;c<right;++c,p+=4) {
                    const double a=double(p[3]);alpha+=a;
                    const double w=straight ? qMax(0.0,a/full) : 1.0;weight+=w;
                    colour[0]+=double(p[0])*w;colour[1]+=double(p[1])*w;colour[2]+=double(p[2])*w;
                }
            }
            for(int k=0;k<3;++k) {
                const double v=weight>0 ? colour[k]/weight : 0.0;
                if constexpr(std::is_floating_point_v<T>) o[k]=T(v);
                else o[k]=T(qBound(0.0,std::round(v),full));
            }
            const double a=alpha/count;
            if constexpr(std::is_floating_point_v<T>) o[3]=T(a);
            else o[3]=T(qBound(0.0,std::round(a),full));
        }
    }
}
}

QImage PlaybackView::preview(const QImage &frame,int divisor) {
    if(frame.isNull() || (divisor!=2 && divisor!=4)) return frame;
    const int width=qMax(1,frame.width()/divisor),height=qMax(1,frame.height()/divisor);
    enum Depth {Byte,Word,Real,Other} depth=Other;bool straight=false;
    switch(frame.format()) {
    case QImage::Format_RGBA8888: case QImage::Format_ARGB32: straight=true; depth=Byte; break;
    case QImage::Format_RGBA8888_Premultiplied: case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888: case QImage::Format_RGB32: depth=Byte; break;
    case QImage::Format_RGBA64: straight=true; depth=Word; break;
    case QImage::Format_RGBA64_Premultiplied: case QImage::Format_RGBX64: depth=Word; break;
    case QImage::Format_RGBA32FPx4: straight=true; depth=Real; break;
    case QImage::Format_RGBA32FPx4_Premultiplied: case QImage::Format_RGBX32FPx4: depth=Real; break;
    default: break;
    }
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
    if(depth==Byte) depth=Other; // ARGB32 keeps alpha in its last byte only on little-endian
#endif
    QImage out;
    if(depth==Other) {
        out=frame.scaled(width,height,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    } else {
        // The display-only reduction for Half/Quarter viewing: banded across
        // the pool (the same pixels on any number of threads), so reducing
        // a 4K frame costs a few milliseconds rather than one core's worth.
        out=QImage(width,height,frame.format());
        if(out.isNull()) return frame.scaled(width,height,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        const int rows=16,bands=(height+rows-1)/rows;
        const auto band=[&](int i) {
            const int y0=i*rows,y1=qMin(height,y0+rows);
            if(depth==Byte) boxRows<uchar>(frame,out,divisor,y0,y1,straight,255.0);
            else if(depth==Word) boxRows<quint16>(frame,out,divisor,y0,y1,straight,65535.0);
            else boxRows<float>(frame,out,divisor,y0,y1,straight,1.0);
            return true;
        };
        if(bands>1 && Bands::displayThreads()) Bands::run(bands,band);
        else for(int i=0;i<bands;++i) band(i);
        out.setColorSpace(frame.colorSpace());
        out.setDotsPerMeterX(frame.dotsPerMeterX()/divisor);out.setDotsPerMeterY(frame.dotsPerMeterY()/divisor);
    }
    // Explicit metadata propagation keeps OCIO identity and non-square pixel
    // geometry independent of Qt codecs.
    for(const auto &key:frame.textKeys()) out.setText(key,frame.text(key));
    return out;
}
