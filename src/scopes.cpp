#include "scopes.h"
#include "bands.h"
#include "imagebands.h"

#include <QColor>
#include <QPainter>
#include <QThreadPool>
#include <array>
#include <cmath>
#include <vector>

namespace Scopes {
namespace {
const QColor kWell(0x0E, 0x14, 0x1C), kGrid(0x29, 0x32, 0x41), kGridStrong(0x3A, 0x46, 0x58);
const QColor kTrace(0xE7, 0xED, 0xF7), kRed(0xEF, 0x44, 0x44), kGreen(0x34, 0xD3, 0x99), kBlue(0x27, 0xC2, 0xFF);
const QColor kClip(0xFB, 0xBF, 0x24);
const std::array<QColor,4> tints{kRed,kGreen,kBlue,kTrace};
enum Kind { Wave=1, Parade=2, Histogram=4, Vector=8, All=15 };

struct Options {
    double intensity=1, low=-.1, high=1.1, kr=.2126, kb=.0722, steps=0;
    double inverseRange=1/1.2, cbGain=1/1.8556, crGain=1/1.5748;
    QString channels="rgb";
    bool graticule=true;
    int width=256, height=256;
    double luma(double r,double g,double b) const {
        // This form keeps neutral RGB exactly neutral, including float input.
        return g+kr*(r-g)+kb*(b-g);
    }
    int bin(double value,int bins) const {
        return int(qBound(0.,(value-low)*inverseRange,1.)*(bins-1)+.5);
    }
    int row(double value) const { return height-1-bin(value,height); }
    bool outside(double value) const { return value<low || value>high; }
    double quantise(double value) const { return steps ? std::round(value*steps)/steps : value; }
};
Options parse(const QVariantMap &m) {
    Options o;
    const double intensity=m.value("intensity",1.).toDouble();
    if(std::isfinite(intensity))o.intensity=qBound(.2,intensity,4.);
    o.channels=m.value("channels","rgb").toString();
    if(m.value("clamp",false).toBool()){o.low=0;o.high=1;}
    o.graticule=m.value("graticule",true).toBool();
    o.width=qBound(64,m.value("width",256).toInt(),2048);
    o.height=qBound(64,m.value("height",256).toInt(),2048);
    const auto space=m.value("outputSpace","rec709/rec709").toString();
    // ITU-R BT.709, BT.601 and BT.2020 non-constant-luminance matrices.
    if(space.startsWith("rec2020/")){o.kr=.2627;o.kb=.0593;}
    else if(space.startsWith("rec601/")){o.kr=.299;o.kb=.114;}
    const auto depth=m.value("bitDepth","Float").toString();
    if(depth=="8 bit")o.steps=255;
    else if(depth=="10 bit")o.steps=1023;
    else if(depth=="12 bit")o.steps=4095;
    o.inverseRange=1/(o.high-o.low);o.cbGain=1/(2*(1-o.kb));o.crGain=1/(2*(1-o.kr));
    return o;
}
QImage well(int w,int h) {
    QImage img(w,h,QImage::Format_ARGB32_Premultiplied);img.fill(kWell);return img;
}

// Counts are 32-bit: a cell or column can never exceed the frame's pixel
// count, and QImage cannot hold 2^32 pixels. Smaller counts keep each
// parallel piece cache resident. Column populations are summed at paint time.
struct Plane {
    int w,h;
    std::vector<quint32> hits,clipped;
    Plane(int width,int height,bool enabled):w(width),h(height),
        hits(enabled?size_t(w)*h:0),clipped(hits.size()) {}
    void hit(int x,int y,bool clip=false) {
        const auto i=size_t(y)*w+x;++hits[i];if(clip)++clipped[i];
    }
    void level(int first,int last,int row,bool clip) {
        for(int x=first;x<=last;++x)hit(x,row,clip);
    }
    void merge(const Plane &other) {
        for(size_t i=0;i<hits.size();++i){hits[i]+=other.hits[i];clipped[i]+=other.clipped[i];}
    }
    void paint(QImage &img,const QColor &tint,double intensity,int xOff=0,double population=0) const {
        std::vector<quint64> columns(static_cast<size_t>(w), 0);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)columns[x]+=hits[size_t(y)*w+x];
        for(int y=0;y<h;++y) {
            auto *line=reinterpret_cast<QRgb *>(img.scanLine(y));
            for(int x=0;x<w;++x) {
                const auto i=size_t(y)*w+x;if(!hits[i])continue;
                // Fixed density gain: a new peak elsewhere cannot dim this trace.
                // Waveform/parade use the population of this source column;
                // vectorscope uses the population of the entire frame.
                const double norm=intensity*(population?256.:16.)/(population?population:double(columns[x]));
                const auto &colour=clipped[i]?kClip:tint;
                const double a=std::min(1.,std::sqrt(double(hits[i])*norm));
                const QRgb under=line[x+xOff];
                line[x+xOff]=qRgb(int(qRed(under)*(1-a)+colour.red()*a),
                                  int(qGreen(under)*(1-a)+colour.green()*a),
                                  int(qBlue(under)*(1-a)+colour.blue()*a));
            }
        }
    }
};
void horizontalGraticule(QImage &img,const Options &o) {
    if(!o.graticule)return;
    QPainter p(&img);
    // The input is decoded, full-range RGB. Its black/white are 0/1;
    // broadcast code values 16/235 must not be applied to it a second time.
    for(double level:{0.,.25,.5,.75,1.}) {
        p.setPen(level==0 || level==1?kGridStrong:kGrid);
        p.drawLine(0,o.row(level),o.width-1,o.row(level));
    }
}

// Integer input has only 256 possible channel levels: their quantisation and
// plot coordinates are computed once, without reducing the sample count.
struct Level { double value; int row,bin; bool clipped; };
struct Column {int first,last,pFirst,pLast;};
// The float value Qt gives each 8-bit level, so byte input measured directly
// equals the same picture converted to float first.
const std::array<float,256> &byteToFloat() {
    static const std::array<float,256> table=[] {
        QImage ramp(256,1,QImage::Format_RGBA8888);
        for(int i=0;i<256;++i){auto *p=ramp.scanLine(0)+i*4;p[0]=p[1]=p[2]=uchar(i);p[3]=255;}
        const auto real=ramp.convertToFormat(QImage::Format_RGBA32FPx4);
        std::array<float,256> t{};
        for(int i=0;i<256;++i)t[i]=reinterpret_cast<const float*>(real.constScanLine(0))[i*4];
        return t;
    }();
    return table;
}
bool scopeThreads() {
    static const bool on=qEnvironmentVariable("OMAEDIT_SCOPE_THREADS")!=QStringLiteral("0");
    return on;
}

struct Analysis {
    Options o;
    int mask,third,vectorWidth;
    bool lumaOnly;
    std::array<bool,4> selected;
    std::array<Plane,4> wave;
    std::array<Plane,3> parade;
    Plane vector;
    std::array<std::vector<quint32>,4> hist,histClipped;
    quint64 pixels=0;
    Analysis(const Options &options,int kinds,int vectorSize):o(options),mask(kinds),third(o.width/3),vectorWidth(vectorSize),
        lumaOnly(o.channels=="rgb" || o.channels=="y"),
        selected{o.channels.contains('r'),o.channels.contains('g'),o.channels.contains('b'),o.channels=="y"},
        wave{Plane(o.width,o.height,(mask&Wave)&&!lumaOnly&&selected[0]),
             Plane(o.width,o.height,(mask&Wave)&&!lumaOnly&&selected[1]),
             Plane(o.width,o.height,(mask&Wave)&&!lumaOnly&&selected[2]),
             Plane(o.width,o.height,(mask&Wave)&&lumaOnly)},
        parade{Plane(third,o.height,mask&Parade),Plane(third,o.height,mask&Parade),Plane(third,o.height,mask&Parade)},
        vector(vectorSize,vectorSize,mask&Vector) {
        if(mask&Histogram)for(int c=0;c<4;++c){hist[c].resize(o.width);histClipped[c].resize(o.width);}
    }
    // Rows are counted in parallel pieces and the integer counts summed, so
    // the pictures do not depend on the number of pieces.
    bool accumulate(const QImage &frame,const std::atomic<bool> *cancelled,int maximumParts=0) {
        if(frame.isNull())return true;
        // Preserve 16-bit and float signal values, including negative values
        // and highlights above 1. Convert only unsupported/premultiplied formats.
        QImage src=frame;
        const bool integer8=src.format()==QImage::Format_RGB32 || src.format()==QImage::Format_ARGB32;
        const bool bytes8=src.format()==QImage::Format_RGBA8888 || src.format()==QImage::Format_RGBX8888;
        const bool integer16=src.format()==QImage::Format_RGBX64 || src.format()==QImage::Format_RGBA64;
        if(!integer8 && !bytes8 && !integer16 && src.format()!=QImage::Format_RGBA32FPx4 && src.format()!=QImage::Format_RGBX32FPx4)
            src=Bands::convertToFormat(src,QImage::Format_RGBA32FPx4);
        std::array<Level,256> lookup{};
        if(integer8 || bytes8)for(int i=0;i<256;++i) {
            const double v=o.quantise(integer8 ? i/255. : double(byteToFloat()[i]));lookup[i]={v,o.row(v),o.bin(v,o.width),o.outside(v)};
        }
        std::vector<Column> columns(src.width());
        for(int x=0;x<src.width();++x) {
            auto &c=columns[x];c.first=qint64(x)*o.width/src.width();
            c.last=qMax(c.first,int(qint64(x+1)*o.width/src.width())-1);
            c.pFirst=qint64(x)*third/src.width();
            c.pLast=qMax(c.pFirst,int(qint64(x+1)*third/src.width())-1);
        }
        const int height=src.height();
        int parts=scopeThreads() ? qMin(qMin(height/64,Bands::pool()->maxThreadCount()+1),16) : 1;
        if(maximumParts>0)parts=qMin(parts,maximumParts);
        // Every piece holds its own counters: keep all of them within a fixed
        // budget, so a large scope raster uses fewer pieces, not more memory.
        const qint64 perPiece=countersBytes();
        if(perPiece>0)parts=int(qMin<qint64>(parts,qMax<qint64>(1,(qint64(64)<<20)/perPiece)));
        if(parts<2)return accumulateRows(src,lookup,columns,integer8,bytes8,integer16,0,height,cancelled);
        std::vector<Analysis> pieces;pieces.reserve(parts);
        for(int i=0;i<parts;++i)pieces.emplace_back(o,mask,vectorWidth);
        const bool ok=Bands::run(parts,[&](int i) {
            return pieces[i].accumulateRows(src,lookup,columns,integer8,bytes8,integer16,int(qint64(i)*height/parts),int(qint64(i+1)*height/parts),cancelled);
        });
        if(!ok)return false;
        for(const auto &piece:pieces)merge(piece);
        return true;
    }
    qint64 countersBytes() const {
        qint64 cells=0;
        for(const auto &p:wave)cells+=qint64(p.hits.size())*2;
        for(const auto &p:parade)cells+=qint64(p.hits.size())*2;
        cells+=qint64(vector.hits.size())*2;
        for(int c=0;c<4;++c)cells+=qint64(hist[c].size())*2;
        return cells*qint64(sizeof(quint32));
    }
    void merge(const Analysis &other) {
        for(int c=0;c<4;++c)wave[c].merge(other.wave[c]);
        for(int c=0;c<3;++c)parade[c].merge(other.parade[c]);
        vector.merge(other.vector);
        for(int c=0;c<4;++c)for(size_t i=0;i<hist[c].size();++i){hist[c][i]+=other.hist[c][i];histClipped[c][i]+=other.histClipped[c][i];}
        pixels+=other.pixels;
    }
    bool accumulateRows(const QImage &src,const std::array<Level,256> &lookup,const std::vector<Column> &columns,
                        bool integer8,bool bytes8,bool integer16,int y0,int y1,const std::atomic<bool> *cancelled) {
        const bool table8=integer8 || bytes8;
        for(int y=y0;y<y1;++y) {
            if(cancelled && *cancelled)return false;
            const auto *p8=reinterpret_cast<const QRgb *>(src.constScanLine(y));
            const auto *bytes=src.constScanLine(y);
            const auto *p16=reinterpret_cast<const QRgba64 *>(p8);
            const auto *pf=reinterpret_cast<const float *>(p8);
            for(int x=0;x<src.width();++x) {
                std::array<double,4> v;
                std::array<Level,3> levels;
                if(integer8){const auto p=p8[x];levels={lookup[qRed(p)],lookup[qGreen(p)],lookup[qBlue(p)]};v={levels[0].value,levels[1].value,levels[2].value,0};}
                else if(bytes8){const auto *p=bytes+x*4;levels={lookup[p[0]],lookup[p[1]],lookup[p[2]]};v={levels[0].value,levels[1].value,levels[2].value,0};}
                else if(integer16){const auto p=p16[x];v={p.red()/65535.,p.green()/65535.,p.blue()/65535.,0};}
                else v={pf[x*4],pf[x*4+1],pf[x*4+2],0};
                if(!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))continue;
                if(!table8)for(int c=0;c<3;++c) {
                    v[c]=o.quantise(v[c]);levels[c]={v[c],o.row(v[c]),o.bin(v[c],o.width),o.outside(v[c])};
                }
                v[3]=o.luma(v[0],v[1],v[2]);++pixels;
                const auto &col=columns[x];
                if(mask&Wave) {
                    if(lumaOnly)wave[3].level(col.first,col.last,o.row(v[3]),o.outside(v[3]));
                    else for(int c=0;c<3;++c)if(selected[c])wave[c].level(col.first,col.last,levels[c].row,levels[c].clipped);
                }
                if(mask&Parade)for(int c=0;c<3;++c)parade[c].level(col.pFirst,col.pLast,levels[c].row,levels[c].clipped);
                if(mask&Histogram)for(int c=0;c<4;++c)if(selected[c]) {
                    const auto bin=c==3?o.bin(v[3],o.width):levels[c].bin;
                    ++hist[c][bin];if(c==3?o.outside(v[3]):levels[c].clipped)++histClipped[c][bin];
                }
                if(mask&Vector) {
                    const double cb=(v[2]-v[3])*o.cbGain,cr=(v[0]-v[3])*o.crGain;
                    vector.hit(qRound(qBound(0.,.5+cb,1.)*(vectorWidth-1)),
                               qRound(qBound(0.,.5-cr,1.)*(vectorWidth-1)),
                               std::abs(cb)>.5 || std::abs(cr)>.5);
                }
            }
        }
        return true;
    }
    // Where the engine's counting pass puts each count (engine::ScopeLayout).
    engine::ScopeLayout layout() const { return layoutOf(mask); }
    engine::ScopeLayout layoutOf(int kinds) const {
        engine::ScopeLayout l;
        l.width=o.width;l.height=o.height;l.third=third;l.vector=vectorWidth;
        l.wave=kinds&Wave;l.parade=kinds&Parade;l.histogram=kinds&Histogram;l.vectorscope=kinds&Vector;
        l.lumaOnly=lumaOnly;for(int c=0;c<4;++c)l.selected[c]=selected[c];
        l.low=o.low;l.high=o.high;l.inverseRange=o.inverseRange;l.kr=o.kr;l.kb=o.kb;
        l.cbGain=o.cbGain;l.crGain=o.crGain;l.steps=o.steps;
        return l;
    }
    // The counts as one table in that layout (planes not counted stay nought), and back.
    std::vector<quint32> cells() const {
        const auto l=layout();
        std::vector<quint32> out(l.cells());
        const auto put=[&out](const Plane &p,size_t at) {
            std::copy(p.hits.begin(),p.hits.end(),out.begin()+at);
            std::copy(p.clipped.begin(),p.clipped.end(),out.begin()+at+p.hits.size());
        };
        for(int c=0;c<4;++c)put(wave[c],l.waveAt(c));
        for(int c=0;c<3;++c)put(parade[c],l.paradeAt(c));
        put(vector,l.vectorAt());
        for(int c=0;c<4;++c) {
            std::copy(hist[c].begin(),hist[c].end(),out.begin()+l.histogramAt(c));
            std::copy(histClipped[c].begin(),histClipped[c].end(),out.begin()+l.histogramAt(c)+o.width);
        }
        out[l.pixelsAt()]=quint32(pixels);
        return out;
    }
    bool take(const std::vector<quint32> &in) {
        const auto l=layout();
        if(in.size()!=l.cells())return false;
        const auto get=[&in](Plane &p,size_t at) {
            std::copy_n(in.begin()+at,p.hits.size(),p.hits.begin());
            std::copy_n(in.begin()+at+p.hits.size(),p.clipped.size(),p.clipped.begin());
        };
        for(int c=0;c<4;++c)get(wave[c],l.waveAt(c));
        for(int c=0;c<3;++c)get(parade[c],l.paradeAt(c));
        get(vector,l.vectorAt());
        for(int c=0;c<4;++c) {
            std::copy_n(in.begin()+l.histogramAt(c),hist[c].size(),hist[c].begin());
            std::copy_n(in.begin()+l.histogramAt(c)+o.width,histClipped[c].size(),histClipped[c].begin());
        }
        pixels=in[l.pixelsAt()];
        return true;
    }
    QImage waveform() const {
        auto img=well(o.width,o.height);horizontalGraticule(img,o);
        if(lumaOnly)wave[3].paint(img,kTrace,o.intensity);
        else for(int c=0;c<3;++c)if(selected[c])wave[c].paint(img,tints[c],o.intensity);
        return img;
    }
    QImage rgbParade() const {
        auto img=well(o.width,o.height);horizontalGraticule(img,o);
        if(o.graticule){QPainter p(&img);p.setPen(kGridStrong);for(int i=1;i<3;++i)p.drawLine(i*third,0,i*third,o.height-1);}
        // A parade always compares R/G/B; choosing Y must never blank it.
        for(int c=0;c<3;++c)parade[c].paint(img,tints[c],o.intensity,c*third);
        return img;
    }
    QImage histogram() const {
        auto img=well(o.width,o.height);QPainter p(&img);
        if(o.graticule)for(double level:{0.,.25,.5,.75,1.}) {
            p.setPen(level==0 || level==1?kGridStrong:kGrid);
            const int x=o.bin(level,o.width);p.drawLine(x,0,x,o.height-1);
        }
        if(!pixels)return img;
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        for(int c=0;c<4;++c)if(selected[c])for(int x=0;x<o.width;++x) {
            if(!hist[c][x])continue;
            // A fixed logarithmic fraction of the frame, independent of all
            // other bins/channels and of image resolution. No auto-ranging.
            const double fraction=double(hist[c][x])/pixels;
            const int height=qMax(1,qRound(std::log1p(fraction*1024)/std::log1p(1024.)*(o.height-1)));
            QColor tint=histClipped[c][x]?kClip:tints[c];tint.setAlphaF(qMin(1.,.55*std::sqrt(o.intensity)));
            p.setPen(tint);p.drawLine(x,o.height-1,x,o.height-height);
        }
        return img;
    }
    QImage vectorscope() const {
        auto img=well(vectorWidth,vectorWidth);
        const double span=vectorWidth-1,cx=span/2;
        auto point=[&](double r,double g,double b) {
            const double y=o.luma(r,g,b);
            return QPointF((.5+(b-y)/(2*(1-o.kb)))*span,(.5-(r-y)/(2*(1-o.kr)))*span);
        };
        if(o.graticule) {
            QPainter p(&img);p.setRenderHint(QPainter::Antialiasing);p.setPen(kGrid);
            p.drawEllipse(QPointF(cx,cx),cx,cx);p.drawEllipse(QPointF(cx,cx),cx*.75,cx*.75);
            p.drawLine(QPointF(cx,0),QPointF(cx,span));p.drawLine(QPointF(0,cx),QPointF(span,cx));
            p.setPen(kGridStrong);
            for(const auto &rgb:std::array<std::array<double,3>,6>{{{.75,0,0},{.75,.75,0},{0,.75,0},{0,.75,.75},{0,0,.75},{.75,0,.75}}}) {
                const auto at=point(rgb[0],rgb[1],rgb[2]);p.drawRect(QRectF(at-QPointF(4,4),QSizeF(8,8)));
            }
            // Approximate flesh-tone hue guide: 33 degrees left of +Cr,
            // between red and yellow, not in the blue/green quadrant.
            p.setPen(QPen(kGrid,1,Qt::DashLine));const double a=33.*M_PI/180.;
            p.drawLine(QPointF(cx,cx),QPointF(cx-std::sin(a)*cx*.9,cx-std::cos(a)*cx*.9));
        }
        if(pixels)vector.paint(img,kTrace,o.intensity,0,double(pixels));
        return img;
    }
};
} // namespace

QImage waveform(const QImage &frame,const QVariantMap &options) {
    Analysis a(parse(options),Wave,0);a.accumulate(frame,nullptr);return a.waveform();
}
QImage parade(const QImage &frame,const QVariantMap &options) {
    Analysis a(parse(options),Parade,0);a.accumulate(frame,nullptr);return a.rgbParade();
}
QImage histogram(const QImage &frame,const QVariantMap &options) {
    Analysis a(parse(options),Histogram,0);a.accumulate(frame,nullptr);return a.histogram();
}
QImage vectorscope(const QImage &frame,const QVariantMap &options) {
    const auto o=parse(options);Analysis a(o,Vector,o.width);a.accumulate(frame,nullptr);return a.vectorscope();
}
QList<QImage> renderAll(const QImage &frame,const QVariantMap &options,int vectorWidth,const std::atomic<bool> *cancelled,int maximumParts) {
    Analysis a(parse(options),All,qBound(64,vectorWidth,2048));
    if(!a.accumulate(frame,cancelled,maximumParts))return {};
    return {a.waveform(),a.rgbParade(),a.histogram(),a.vectorscope()};
}
engine::ScopeLayout layout(const QVariantMap &options,int vectorWidth) {
    return Analysis(parse(options),0,qBound(64,vectorWidth,2048)).layoutOf(All);
}
std::vector<quint32> counts(const QImage &frame,const QVariantMap &options,int vectorWidth,const std::atomic<bool> *cancelled,int maximumParts) {
    Analysis a(parse(options),All,qBound(64,vectorWidth,2048));
    if(!a.accumulate(frame,cancelled,maximumParts))return {};
    return a.cells();
}
QList<QImage> renderCounted(const std::vector<quint32> &cells,const QVariantMap &options,int vectorWidth) {
    Analysis a(parse(options),All,qBound(64,vectorWidth,2048));
    if(!a.take(cells))return {};
    return {a.waveform(),a.rgbParade(),a.histogram(),a.vectorscope()};
}
} // namespace Scopes
