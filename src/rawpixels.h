#pragma once
#include <QImage>
#include <QVariantMap>
#include <QtEndian>
#include <cmath>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#if __has_include(<libavutil/raw_color_params.h>)
#include <libavutil/raw_color_params.h>
#define OMAEDIT_RAW_COLOR_PARAMS 1
#endif
}

namespace RawPixels {
inline QImage display(const QImage &linear, bool sixteenBit = false) {
    if (linear.isNull() || linear.format()!=QImage::Format_RGBA32FPx4) return {};
    QImage result(linear.size(),sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (result.isNull()) return {};
    for (int y=0;y<linear.height();++y) {
        const auto *source=reinterpret_cast<const float *>(linear.constScanLine(y));
        auto *bytes=result.scanLine(y);
        auto *words=reinterpret_cast<QRgba64 *>(bytes);
        for (int x=0;x<linear.width();++x,source+=4) {
            quint16 rgb[3];
            for (int c=0;c<3;++c) {
                const double v=qMax(0.,double(source[c]));
                const double encoded=v<.018 ? v*4.5 : 1.099*std::pow(v,.45)-.099;
                rgb[c]=quint16(qBound(0.,std::round(encoded*65535),65535.));
            }
            if (sixteenBit) words[x]=QRgba64::fromRgba64(rgb[0],rgb[1],rgb[2],65535);
            else { bytes[4*x]=rgb[0]/257; bytes[4*x+1]=rgb[1]/257; bytes[4*x+2]=rgb[2]/257; bytes[4*x+3]=255; }
        }
    }
    return result;
}
inline QImage develop(const AVFrame *frame, const QVariantMap &settings, QString *error) {
#ifdef OMAEDIT_RAW_COLOR_PARAMS
    const auto *side = av_frame_get_side_data(frame, AV_FRAME_DATA_RAW_COLOR_PARAMS);
    if (!side || side->size < sizeof(AVRawColorParams)) {
        *error = "RAW camera colour metadata is missing; the source cannot be developed accurately.";
        return {};
    }
    const auto &raw = *reinterpret_cast<const AVRawColorParams *>(side->data);
    if (raw.type != AV_RAW_COLOR_PARAMS_PRORES_RAW) { *error = "Unsupported RAW camera colour metadata."; return {}; }
    const int format = frame->format;
    int pattern[4];
    // Sensor positions: top-left, top-right, bottom-left, bottom-right; RGB=0,1,2.
    if (format == AV_PIX_FMT_BAYER_RGGB16LE || format == AV_PIX_FMT_BAYER_RGGB16BE) { pattern[0]=0; pattern[1]=1; pattern[2]=1; pattern[3]=2; }
    else if (format == AV_PIX_FMT_BAYER_BGGR16LE || format == AV_PIX_FMT_BAYER_BGGR16BE) { pattern[0]=2; pattern[1]=1; pattern[2]=1; pattern[3]=0; }
    else if (format == AV_PIX_FMT_BAYER_GRBG16LE || format == AV_PIX_FMT_BAYER_GRBG16BE) { pattern[0]=1; pattern[1]=0; pattern[2]=2; pattern[3]=1; }
    else if (format == AV_PIX_FMT_BAYER_GBRG16LE || format == AV_PIX_FMT_BAYER_GBRG16BE) { pattern[0]=1; pattern[1]=2; pattern[2]=0; pattern[3]=1; }
    else { *error = "Unsupported RAW sensor layout."; return {}; }
    const double black = av_q2d(raw.black_level), white = av_q2d(raw.white_level);
    const auto &camera = raw.codec.prores_raw;
    const double exposure = settings.value("rawExposure", 0.).toDouble();
    const double wb[3]{av_q2d(camera.wb_red)*settings.value("rawRed",1.).toDouble(), 1.,
                       av_q2d(camera.wb_blue)*settings.value("rawBlue",1.).toDouble()};
    const double gain = av_q2d(camera.gain)*std::exp2(exposure);
    if (!std::isfinite(black) || !std::isfinite(white) || white <= black || !std::isfinite(gain)
        || gain <= 0 || !std::isfinite(wb[0]) || !std::isfinite(wb[2]) || wb[0] <= 0 || wb[2] <= 0) {
        *error = "Invalid RAW exposure, white balance or sensor levels."; return {};
    }
    // FFmpeg's camera matrix lands in XYZ D65. The editor's interchange is linear Rec.709.
    constexpr double xyzToRgb[3][3]{{3.2409699419,-1.5373831776,-.4986107603},
                                   {-.9692436363,1.8759675015,.0415550574},
                                   {.0556300797,-.2039769589,1.0569715142}};
    double matrix[3][3]{};
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) for (int k=0;k<3;++k) {
        const double value = av_q2d(camera.color_matrix[k][j]);
        if (!std::isfinite(value)) { *error = "Invalid RAW camera colour matrix."; return {}; }
        matrix[i][j] += xyzToRgb[i][k]*value*gain;
    }
    if (frame->width < 2 || frame->height < 2 || qint64(frame->width)*frame->height > 64000000) {
        *error = "Invalid or oversized RAW frame."; return {};
    }
    QImage out(frame->width,frame->height,QImage::Format_RGBA32FPx4);
    if (out.isNull()) { *error = "Cannot allocate the developed RAW frame."; return {}; }
    const bool big = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format))->flags & AV_PIX_FMT_FLAG_BE;
    auto sensor = [&](int x,int y) {
        const auto *p = frame->data[0]+y*frame->linesize[0]+x*2;
        return ((big ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p))/65535.0-black)/(white-black);
    };
    for (int y=0;y<out.height();++y) {
        auto *pixel = reinterpret_cast<float *>(out.scanLine(y));
        for (int x=0;x<out.width();++x,pixel+=4) {
            double rgb[3]{}; int count[3]{};
            const int own = pattern[(y&1)*2+(x&1)];
            // Bilinear demosaic. At the boundary use only real sensor sites, avoiding parity flips.
            for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
                const int xx=x+dx, yy=y+dy;
                if (xx<0 || yy<0 || xx>=out.width() || yy>=out.height()) continue;
                const int channel=pattern[(yy&1)*2+(xx&1)];
                if (channel==own && (dx || dy)) continue;
                rgb[channel]+=sensor(xx,yy); ++count[channel];
            }
            for (int c=0;c<3;++c) rgb[c]=rgb[c]/count[c]*wb[c];
            for (int c=0;c<3;++c) pixel[c]=matrix[c][0]*rgb[0]+matrix[c][1]*rgb[1]+matrix[c][2]*rgb[2];
            pixel[3]=1.f;
        }
    }
    out.setText("omaedit.inputLinearRec709", "1");
    return out;
#else
    Q_UNUSED(frame); Q_UNUSED(settings);
    *error = "ProRes RAW colour development requires FFmpeg 9 or newer.";
    return {};
#endif
}
}
