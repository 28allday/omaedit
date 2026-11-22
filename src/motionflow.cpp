#include "motionflow.h"
#include <QCoreApplication>
#include <QColorSpace>
#include <QCache>
#include <QCryptographicHash>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry/2d.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace {
using Pixel = cv::Vec4f;
constexpr int analysisLimit=960;
struct Cache {
    std::mutex mutex;
    QCache<QByteArray,std::shared_ptr<const MotionFlow>> fields{MotionFlow::cacheLimit()};
};
Cache &cache(){static Cache instance;return instance;}
void fail(QString *error,const char *message) {
    if(error)*error=QCoreApplication::translate("MotionFlow",message);
}
bool cancelled(const MotionFlow::Cancel &cancel,QString *error) {
    if(!cancel || !cancel())return false;
    fail(error,QT_TRANSLATE_NOOP("MotionFlow","Motion interpolation cancelled."));return true;
}
bool supported(QImage::Format format) {
    return format==QImage::Format_RGBA8888 || format==QImage::Format_RGBA64 || format==QImage::Format_RGBA32FPx4;
}
bool validPair(const QImage &a,const QImage &b,QString *error) {
    if(a.isNull() || b.isNull() || a.size()!=b.size() || a.format()!=b.format() || !supported(a.format())) {
        fail(error,QT_TRANSLATE_NOOP("MotionFlow","Motion interpolation needs two matching RGBA pictures."));return false;
    }
    return true;
}
Pixel read(const QImage &image,int x,int y) {
    const auto *row=image.constScanLine(y);
    Pixel p;
    if(image.format()==QImage::Format_RGBA8888) {
        row+=x*4;for(int c=0;c<4;++c)p[c]=row[c]/255.f;
    } else if(image.format()==QImage::Format_RGBA64) {
        const auto *v=reinterpret_cast<const quint16 *>(row)+x*4;
        for(int c=0;c<4;++c)p[c]=v[c]/65535.f;
    } else {
        const auto *v=reinterpret_cast<const float *>(row)+x*4;
        for(int c=0;c<4;++c)p[c]=v[c];
    }
    return p;
}
void write(QImage &image,int x,int y,const Pixel &pixel) {
    auto *row=image.scanLine(y);
    if(image.format()==QImage::Format_RGBA8888) {
        row+=x*4;for(int c=0;c<4;++c)row[c]=quint8(std::lround(std::clamp(pixel[c],0.f,1.f)*255.f));
    } else if(image.format()==QImage::Format_RGBA64) {
        auto *v=reinterpret_cast<quint16 *>(row)+x*4;
        for(int c=0;c<4;++c)v[c]=quint16(std::lround(std::clamp(pixel[c],0.f,1.f)*65535.f));
    } else {
        auto *v=reinterpret_cast<float *>(row)+x*4;
        for(int c=0;c<4;++c)v[c]=pixel[c];
    }
}
template<typename T> T sample(const cv::Mat &image,float x,float y) {
    x=std::clamp(x,0.f,float(image.cols-1));y=std::clamp(y,0.f,float(image.rows-1));
    const int ix=int(x),iy=int(y),jx=std::min(ix+1,image.cols-1),jy=std::min(iy+1,image.rows-1);
    const float u=x-ix,v=y-iy;
    return (image.at<T>(iy,ix)*(1-u)+image.at<T>(iy,jx)*u)*(1-v)
        +(image.at<T>(jy,ix)*(1-u)+image.at<T>(jy,jx)*u)*v;
}
bool analysisImage(const QImage &image,cv::Size size,cv::Mat &gray,cv::Mat &colour,
                   QString *error,const MotionFlow::Cancel &cancel) {
    gray.create(size,CV_8UC1);colour.create(size,CV_32FC4);
    for(int y=0;y<size.height;++y) {
        if(cancelled(cancel,error))return false;
        const int top=y*image.height()/size.height,bottom=std::max(top+1,(y+1)*image.height()/size.height);
        for(int x=0;x<size.width;++x) {
            const int left=x*image.width()/size.width,right=std::max(left+1,(x+1)*image.width()/size.width);
            Pixel sum(0,0,0,0);float luma=0;
            for(int yy=top;yy<bottom;++yy)for(int xx=left;xx<right;++xx) {
                auto p=read(image,xx,yy);
                for(int c=0;c<4;++c)if(!std::isfinite(p[c])) {
                    fail(error,QT_TRANSLATE_NOOP("MotionFlow","Motion interpolation received a non-finite pixel."));return false;
                }
                p[3]=std::clamp(p[3],0.f,1.f);
                const float light=std::max(0.f,(p[0]*.2126f+p[1]*.7152f+p[2]*.0722f)*p[3]);
                luma+=.9f*light/(1+light)+.1f*p[3];
                for(int c=0;c<3;++c) {const float v=p[c]*p[3];sum[c]+=v/(1+std::abs(v));}
                sum[3]+=p[3];
            }
            const float count=(right-left)*(bottom-top);
            gray.at<uchar>(y,x)=uchar(std::lround(255*std::clamp(luma/count,0.f,1.f)));
            colour.at<Pixel>(y,x)=sum/count;
        }
    }
    return true;
}
}

struct MotionFlow::State {
    QSize size;
    QImage::Format format=QImage::Format_Invalid;
    cv::Mat motion[2],confidence[2];
    bool cut=false;
    bool blend=false;
    float maxVertical=0;
};
MotionFlow::MotionFlow():d(std::make_unique<State>()) {}
MotionFlow::~MotionFlow()=default;
void MotionFlow::clearCache() {auto &c=cache();std::lock_guard<std::mutex> lock(c.mutex);c.fields.clear();}
qsizetype MotionFlow::cacheBytes() {auto &c=cache();std::lock_guard<std::mutex> lock(c.mutex);return c.fields.totalCost();}
std::shared_ptr<const MotionFlow> MotionFlow::cached(const QImage &first,const QImage &second,
    QString *error,const Cancel &cancel,const QByteArray &madeFrom) {
    if(error)error->clear();
    if(!validPair(first,second,error) || cancelled(cancel,error))return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const int shape[]={first.width(),first.height(),int(first.format())};
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(shape),sizeof(shape)));
    // What the pair was made from, when the caller vouches for it: hashing two
    // UHD pictures costs more than looking the field up saves.
    if(!madeFrom.isEmpty()){hash.addData(QByteArrayView("made from"));hash.addData(madeFrom);}
    else for(const auto *image:{&first,&second})for(int y=0;y<image->height();++y) {
        if((y&31)==0 && cancelled(cancel,error))return {};
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image->constScanLine(y)),image->width()*image->depth()/8));
    }
    const auto key=hash.result();auto &c=cache();
    {std::lock_guard<std::mutex> lock(c.mutex);if(auto *found=c.fields.object(key))return *found;}
    auto result=analyse(first,second,error,cancel);
    if(!result || cancelled(cancel,error))return {};
    {std::lock_guard<std::mutex> lock(c.mutex);
        if(auto *found=c.fields.object(key))return *found;
        c.fields.insert(key,new std::shared_ptr<const MotionFlow>(result),int(result->memoryBytes()));
    }
    return result;
}
bool MotionFlow::sceneCut() const{return d->cut;}
QSize MotionFlow::analysisSize() const{return {d->motion[0].cols,d->motion[0].rows};}
qsizetype MotionFlow::memoryBytes() const {
    qsizetype bytes=0;for(int i=0;i<2;++i)bytes+=d->motion[i].total()*d->motion[i].elemSize()+d->confidence[i].total()*d->confidence[i].elemSize();return bytes;
}
std::shared_ptr<const MotionFlow> MotionFlow::analyse(const QImage &first,const QImage &second,
    QString *error,const Cancel &cancel) {
    if(error)error->clear();
    if(!validPair(first,second,error) || cancelled(cancel,error))return {};
    try {
        static std::once_flag threads;
        std::call_once(threads,[]{cv::setNumThreads(2);});
        auto result=std::shared_ptr<MotionFlow>(new MotionFlow);
        auto &d=*result->d;d.size=first.size();d.format=first.format();
        const double scale=std::min(1.,double(analysisLimit)/std::max(first.width(),first.height()));
        const cv::Size size(std::max(32,int(std::lround(first.width()*scale))),std::max(32,int(std::lround(first.height()*scale))));
        cv::Mat gray[2],colour[2];
        if(!analysisImage(first,size,gray[0],colour[0],error,cancel)
            || !analysisImage(second,size,gray[1],colour[1],error,cancel))return {};
        const bool still=cv::norm(colour[0],colour[1],cv::NORM_INF)<1e-7;
        auto estimator=cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_MEDIUM);
        for(int i=0;i<2;++i) {
            if(cancelled(cancel,error))return {};
            if(still)d.motion[i]=cv::Mat::zeros(size,CV_32FC2);
            else estimator->calc(gray[i],gray[1-i],d.motion[i]);
            d.confidence[i].create(size,CV_32FC1);
        }
        double difference=0,unwarped=0,changedDifference=0,changedUnwarped=0;
        int bad=0,checked=0,changed=0,changedBad=0;
        for(int i=0;i<2;++i)for(int y=0;y<size.height;++y) {
            if(cancelled(cancel,error))return {};
            for(int x=0;x<size.width;++x) {
                const auto motion=d.motion[i].at<cv::Vec2f>(y,x);
                const float xx=x+motion[0],yy=y+motion[1];
                const auto back=sample<cv::Vec2f>(d.motion[1-i],xx,yy);
                const auto residual=motion+back;
                const float mismatch=residual.dot(residual);
                d.confidence[i].at<float>(y,x)=std::max(.001f,std::exp(-.5f*mismatch));
                d.maxVertical=std::max(d.maxVertical,std::abs(motion[1])*first.height()/size.height);
                if(xx<0 || yy<0 || xx>size.width-1 || yy>size.height-1)continue;
                const auto a=colour[i].at<Pixel>(y,x),b=sample<Pixel>(colour[1-i],xx,yy),c=colour[1-i].at<Pixel>(y,x);
                float delta=0,plain=0;
                for(int ch=0;ch<3;++ch){delta+=std::abs(a[ch]-b[ch])/3;plain+=std::abs(a[ch]-c[ch])/3;}
                delta=std::max(delta,std::abs(a[3]-b[3]));plain=std::max(plain,std::abs(a[3]-c[3]));
                difference+=delta;unwarped+=plain;bad+=mismatch>4;++checked;
                if(plain>.02f) {
                    changedDifference+=delta;changedUnwarped+=plain;
                    changedBad+=mismatch>4;++changed;
                }
            }
        }
        // A cut should switch pictures, not manufacture a moving hybrid.
        d.cut=checked && (difference/checked>.18
            || (difference/checked>.08 && difference>unwarped*.8 && bad>checked*.6));
        // Static backgrounds must not conceal a failed estimate on moving
        // subjects. Require both inconsistent motion and little photometric
        // improvement before falling back; occlusion alone is not a failure.
        // The decision belongs to the pair, so every requested phase agrees.
        d.blend=!d.cut && changed>=64 && changedBad>changed*.6
            && changedDifference>changedUnwarped*.8;
        return result;
    } catch(const cv::Exception &) {fail(error,QT_TRANSLATE_NOOP("MotionFlow","Motion analysis could not be completed."));}
      catch(const std::bad_alloc &) {fail(error,QT_TRANSLATE_NOOP("MotionFlow","There is not enough memory for motion analysis."));}
    return {};
}

QImage MotionFlow::interpolate(const QImage &first,const QImage &second,double phase,
    QString *error,const Cancel &cancel) const {
    if(error)error->clear();
    if(!validPair(first,second,error) || cancelled(cancel,error))return {};
    if(first.size()!=d->size || first.format()!=d->format || !std::isfinite(phase) || phase<0 || phase>1) {
        fail(error,QT_TRANSLATE_NOOP("MotionFlow","The motion field does not match the requested pictures or phase."));return {};
    }
    if(phase==0 || phase==1 || d->cut)return (phase<.5?first:second).copy();
    try {
        QImage output(first.size(),first.format());
        if(output.isNull()){fail(error,QT_TRANSLATE_NOOP("MotionFlow","There is not enough memory for motion interpolation."));return {};}
        output.setColorSpace(first.colorSpace());
        output.setDotsPerMeterX(first.dotsPerMeterX());output.setDotsPerMeterY(first.dotsPerMeterY());
        for(const auto &key:first.textKeys())output.setText(key,first.text(key));
        const int width=first.width(),height=first.height();
        if(d->blend) {
            const float t=float(phase);
            for(int y=0;y<height;++y) {
                if((y&15)==0 && cancelled(cancel,error))return {};
                for(int x=0;x<width;++x) {
                    auto a=read(first,x,y),b=read(second,x,y);
                    a[3]=std::clamp(a[3],0.f,1.f);b[3]=std::clamp(b[3],0.f,1.f);
                    Pixel p(0,0,0,a[3]*(1-t)+b[3]*t);
                    if(p[3]>0)for(int c=0;c<3;++c)p[c]=(a[c]*a[3]*(1-t)+b[c]*b[3]*t)/p[3];
                    write(output,x,y,p);
                }
            }
            return output;
        }
        // Bound scratch storage independently of image height. Only source
        // rows whose motion can reach this tile need to be visited.
        const int rows=std::clamp(int(32*1024*1024/(qsizetype(width)*5*sizeof(float))),1,256);
        const float sx=float(d->motion[0].cols)/width,sy=float(d->motion[0].rows)/height;
        using Sum=cv::Vec<float,5>;
        for(int top=0;top<height;top+=rows) {
            if(cancelled(cancel,error))return {};
            const int count=std::min(rows,height-top),bottom=top+count;
            std::vector<Sum> sums(qsizetype(width)*count,Sum::all(0));
            for(int direction=0;direction<2;++direction) {
                const auto &source=direction?second:first;
                const float travel=direction?1-phase:phase,fade=1-travel;
                const int reach=int(std::ceil(d->maxVertical*travel))+2;
                for(int y=std::max(0,top-reach);y<std::min(height,bottom+reach);++y) {
                    if((y&15)==0 && cancelled(cancel,error))return {};
                    for(int x=0;x<width;++x) {
                        const float ax=(x+.5f)*sx-.5f,ay=(y+.5f)*sy-.5f;
                        const auto flow=sample<cv::Vec2f>(d->motion[direction],ax,ay);
                        const float xx=x+travel*flow[0]/sx,yy=y+travel*flow[1]/sy;
                        if(xx<=-1 || xx>=width || yy<=top-1 || yy>=bottom)continue;
                        const int ix=int(std::floor(xx)),iy=int(std::floor(yy));
                        const float fx=xx-ix,fy=yy-iy;
                        const float confidence=sample<float>(d->confidence[direction],ax,ay)*fade;
                        auto p=read(source,x,y);p[3]=std::clamp(p[3],0.f,1.f);
                        for(int c=0;c<3;++c)p[c]*=p[3];
                        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
                            const int px=ix+dx,py=iy+dy;
                            if(px<0 || px>=width || py<top || py>=bottom)continue;
                            const float weight=confidence*(dx?fx:1-fx)*(dy?fy:1-fy);
                            auto &sum=sums[(py-top)*width+px];
                            for(int c=0;c<4;++c)sum[c]+=p[c]*weight;
                            sum[4]+=weight;
                        }
                    }
                }
            }
            cv::Mat holes(count,width,CV_8UC1);std::vector<int> valid;
            for(int y=0;y<count;++y)for(int x=0;x<width;++x) {
                const int at=y*width+x;const auto &s=sums[at];Pixel p(0,0,0,0);
                holes.at<uchar>(y,x)=s[4]<1e-8f;
                if(s[4]>=1e-8f) {
                    valid.push_back(at);
                    for(int c=0;c<3;++c)p[c]=s[3]>0?s[c]/s[3]:0;
                    p[3]=std::clamp(s[3]/s[4],0.f,1.f);
                    write(output,x,top+y,p);
                }
            }
            if(valid.size()!=sums.size()) {
                cv::Mat distance,labels;
                if(!valid.empty())cv::distanceTransform(holes,distance,labels,cv::DIST_L2,5,cv::DIST_LABEL_PIXEL);
                for(int y=0;y<count;++y)for(int x=0;x<width;++x)if(holes.at<uchar>(y,x)) {
                    Pixel p;
                    if(valid.empty())p=read(phase<.5?first:second,x,top+y);
                    else {
                        const int at=valid[labels.at<int>(y,x)-1];p=read(output,at%width,top+at/width);
                    }
                    write(output,x,top+y,p);
                }
            }
        }
        return output;
    } catch(const cv::Exception &) {fail(error,QT_TRANSLATE_NOOP("MotionFlow","Motion interpolation could not be completed."));}
      catch(const std::bad_alloc &) {fail(error,QT_TRANSLATE_NOOP("MotionFlow","There is not enough memory for motion interpolation."));}
    return {};
}
