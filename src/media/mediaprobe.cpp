#include "mediaprobe.h"

#include "animatedwebp.h"
#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>

namespace media {

namespace {

QString tagOf(AVDictionary *dict, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? QString::fromUtf8(e->value).trimmed() : QString();
}

// A rate as a number; none (0/0, as a stream without a frame rate carries:
// an animated WebP) is 0, not a division by zero.
double rateOf(AVRational rate) { return rate.den != 0 ? av_q2d(rate) : 0.0; }

// The video stream's rate (libavformat's guess_frame_rate).
AVRational guessRate(AVFormatContext *format, AVStream *stream) {
    AVRational rate = av_guess_frame_rate(format, stream, nullptr);
    double fps = rateOf(rate);
    if (std::isnan(fps) || std::isinf(fps) || fps == 0) {
        rate = stream->avg_frame_rate;
        fps = rateOf(rate);
    }
    // A container clock (1000 fps) over a valid average: the average, rationalised.
    if (rateOf(stream->r_frame_rate) >= 1000 && rateOf(stream->avg_frame_rate) > 0) {
        rate = av_d2q(rateOf(stream->avg_frame_rate), 1024);
        fps = rateOf(rate);
    }
    if (std::isnan(fps) || std::isinf(fps) || fps < 1.0) rate = AVRational{25, 1};
    // Broadcast rates as Matroska rounds them.
    if (format->iformat && format->iformat->name && std::strstr(format->iformat->name, "matroska")) {
        switch (std::lrint(100000.0 * rate.num / rate.den)) {
        case 2997003: rate = {30000, 1001}; break;
        case 5994006: rate = {60000, 1001}; break;
        case 2397602: rate = {24000, 1001}; break;
        case 4795204: rate = {48000, 1001}; break;
        default: break;
        }
    }
    const double capture = tagOf(format->metadata, "com.android.capture.fps").toDouble();
    if (capture > 0.0 && std::isfinite(capture)) rate = av_d2q(capture, 1024);
    return rate;
}

// The ITU matrix a stream is encoded with, guessed from its size when untagged.
int matrixOf(AVColorSpace space, int width, int height) {
    switch (space) {
    case AVCOL_SPC_RGB: return 0;
    case AVCOL_SPC_BT709: return 709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return 601;
    case AVCOL_SPC_SMPTE240M: return 240;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return 2020;
    default: break;
    }
    Q_UNUSED(width);
    return SuiteVideo::untaggedIsBt601(height) ? 601 : 709;  // the one rule every reader shares
}

double rotationOf(AVStream *stream) {
    const AVPacketSideData *sd = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    double theta = 0;
    if (sd && sd->size >= int(9 * sizeof(int32_t))) theta = -av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
    else if (const QString r = tagOf(stream->metadata, "rotate"); !r.isEmpty()) theta = r.toDouble();
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    return theta;
}

// Bit depth from an FFmpeg pixel-format name: yuv420p → 8, yuv422p10le → 10,
// gbrp12 → 12, rgb48 → 16. Zero when there is no name.
int depthOfPixelFormat(const QString &pixFmt) {
    if (pixFmt.isEmpty()) return 0;
    static const QRegularExpression digits(QStringLiteral("(\\d+)(?:le|be)?$"));
    const QRegularExpressionMatch m = digits.match(pixFmt);
    if (m.hasMatch()) {
        const int n = m.captured(1).toInt();
        if (n == 9 || n == 10 || n == 12 || n == 14 || n == 16) return n;
        if (n == 48 || n == 64) return n / (n == 48 ? 3 : 4);
    }
    return 8;
}

int depthOfSampleFormat(const QString &sampleFmt) {
    if (sampleFmt.startsWith(QStringLiteral("u8"))) return 8;
    if (sampleFmt.startsWith(QStringLiteral("s16"))) return 16;
    if (sampleFmt.startsWith(QStringLiteral("s32")) || sampleFmt.startsWith(QStringLiteral("flt"))) return 32;
    if (sampleFmt.startsWith(QStringLiteral("s64")) || sampleFmt.startsWith(QStringLiteral("dbl"))) return 64;
    return 0;
}

}  // namespace

bool isStillPath(const QString &path) {
    static const QStringList stills{"png", "jpg", "jpeg", "tif", "tiff", "exr", "webp", "bmp", "psd"};
    return stills.contains(QFileInfo(path).suffix().toLower()) && !AnimatedWebP::inspect(path).animated;
}

Probe probe(const QString &path) {
    Probe p;
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        p.error = QStringLiteral("The file cannot be read.");
        return p;
    }
    AVFormatContext *format = nullptr;
    const QByteArray file = QFile::encodeName(path);
    int got = avformat_open_input(&format, file.constData(), nullptr, nullptr);
    if (got >= 0) got = avformat_find_stream_info(format, nullptr);
    if (got < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(got, text, sizeof text);
        p.error = QString::fromUtf8(text);
        if (format) avformat_close_input(&format);
        return p;
    }
    p.valid = true;
    p.seconds = format->duration != AV_NOPTS_VALUE ? double(format->duration) / AV_TIME_BASE : 0;
    p.duration = format->duration != AV_NOPTS_VALUE ? format->duration : -1;
    QVariantMap &meta = p.metadata;
    AVStream *video = nullptr;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVStream *s = format->streams[i];
        const AVCodecParameters *par = s->codecpar;
        // Named by the decoder that reads it ("mp3float"), as it always has been.
        const AVCodecDescriptor *codec = avcodec_descriptor_get(par->codec_id);
        const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
        const QString codecName = decoder ? QString::fromLatin1(decoder->name) : codec ? QString::fromLatin1(codec->name) : QString();
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
            if (!video) video = s;
            if (!meta.contains(QStringLiteral("videoCodec"))) {
                const QString pixFmt = par->format >= 0 ? QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(par->format))) : QString();
                meta.insert(QStringLiteral("videoCodec"), codecName);
                meta.insert(QStringLiteral("videoCodecLong"), decoder && decoder->long_name ? QString::fromLatin1(decoder->long_name)
                                                              : codec && codec->long_name ? QString::fromLatin1(codec->long_name) : QString());
                meta.insert(QStringLiteral("pixelFormat"), pixFmt);
                meta.insert(QStringLiteral("bitDepth"), depthOfPixelFormat(pixFmt));
                if (meta.value(QStringLiteral("videoCodec")).toString() == QLatin1String("prores_raw")) meta[QStringLiteral("bitDepth")] = 16;
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->ch_layout.nb_channels > 0) {
            p.hasAudio = true;
            if (!meta.contains(QStringLiteral("audioCodec"))) {
                const QString sampleFmt = par->format >= 0 ? QString::fromLatin1(av_get_sample_fmt_name(AVSampleFormat(par->format))) : QString();
                meta.insert(QStringLiteral("audioCodec"), codecName);
                meta.insert(QStringLiteral("channels"), par->ch_layout.nb_channels);
                meta.insert(QStringLiteral("sampleRate"), par->sample_rate);
                meta.insert(QStringLiteral("sampleFormat"), sampleFmt);
                // The depth the file stores: a 24-bit WAV says 24 though it
                // decodes to 32-bit samples. Lossy sound (AAC, MP3, Opus)
                // stores no depth, so it says none.
                int depth = par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample : av_get_exact_bits_per_sample(par->codec_id);
                if (depth <= 0 && (par->codec_id == AV_CODEC_ID_FLAC || par->codec_id == AV_CODEC_ID_ALAC)) depth = depthOfSampleFormat(sampleFmt);
                if (depth > 0) meta.insert(QStringLiteral("audioBitDepth"), depth);
            }
        }
    }
    const auto animation = AnimatedWebP::inspect(path);
    p.still = isStillPath(path);
    if (video) {
        p.hasVideo = true;
        const AVCodecParameters *par = video->codecpar;
        AVRational rate = guessRate(format, video);
        AVRational sar = video->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0) sar = par->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0) sar = {1, 1};
        p.rotation = rotationOf(video);
        int w = par->width, h = par->height;
        if (std::abs(p.rotation - 90) < 1 || std::abs(p.rotation - 270) < 1) {
            std::swap(w, h);
            sar = {sar.den, sar.num};
        }
        p.width = w;
        p.height = h;
        p.sarNum = sar.num;
        p.sarDen = sar.den;
        p.progressive = par->field_order == AV_FIELD_PROGRESSIVE || par->field_order == AV_FIELD_UNKNOWN;
        p.colourspace = matrixOf(par->color_space, par->width, par->height);
        // The profile's rule: a rate above 1000 is a stream clock (60 then);
        // AVCHD's doubled interlaced rate halves.
        if (1000 > av_q2d(rate)) { p.fpsNum = rate.num; p.fpsDen = rate.den; }
        else { p.fpsNum = 60; p.fpsDen = 1; }
        if (!p.progressive && (p.fpsNum / p.fpsDen == 50 || p.fpsNum / p.fpsDen == 59)) p.fpsNum /= 2;
        if (par->codec_id == AV_CODEC_ID_PRORES_RAW || par->codec_id == AV_CODEC_ID_WEBP) {
            if (par->width > 0 && par->height > 0) {
                p.width = par->width;
                p.height = par->height;
                p.sarNum = p.sarDen = 1;
                p.progressive = true;
            }
            const double fps = rateOf(video->r_frame_rate);
            if (fps > 0 && fps <= 240) { p.fpsNum = int(std::lrint(fps * 1000)); p.fpsDen = 1000; }
        }
    }
    if (animation.animated) p.animationMilliseconds = animation.milliseconds;
    if (animation.animated && animation.tick > 0) {
        p.fpsNum = 1000;
        p.fpsDen = animation.tick;
    }
    if (p.still) {
        // A still's own size from its header when the container has none.
        QSize size(video ? p.width : 0, video ? p.height : 0);
        if (size.isEmpty()) size = QImageReader(path).size();
        if (!size.isEmpty()) {
            p.width = size.width();
            p.height = size.height();
            p.sarNum = p.sarDen = 1;
            p.progressive = true;
        }
    }
    // The length on the file's clock.
    const double fps = p.fps();
    p.frames = framesOn(p, fps);
    meta.insert(QStringLiteral("fpsNum"), p.fpsNum);
    meta.insert(QStringLiteral("fpsDen"), p.fpsDen);
    meta.insert(QStringLiteral("pixelAspect"), QStringLiteral("%1:%2").arg(p.sarNum).arg(p.sarDen));
    // Camera facts, verbatim with their units, when the container has them.
    const QMap<QString, QStringList> cameraKeys{
        {"lens", {"lens_model", "lens", "com.apple.quicktime.camera.lens_model"}},
        {"focalLength", {"focal_length", "focal_length_in_35mm_film"}},
        {"focusDistance", {"focus_distance", "subject_distance"}},
        {"aperture", {"f_number", "aperture", "aperture_value"}},
        {"iso", {"iso", "iso_speed", "iso_speed_ratings"}},
        {"shutter", {"exposure_time", "shutter_speed", "shutter_speed_value"}},
        {"whiteBalance", {"white_balance", "color_temperature"}}};
    for (auto it = cameraKeys.constBegin(); it != cameraKeys.constEnd(); ++it)
        for (const QString &tag : it.value())
            if (const QString v = tagOf(format->metadata, tag.toUtf8().constData()); !v.isEmpty()) { meta.insert(it.key(), v); break; }
    if (video && p.colourspace > 0) meta.insert(QStringLiteral("colorSpace"), QStringLiteral("Rec. %1").arg(p.colourspace));
    if (const QString created = tagOf(format->metadata, "creation_time"); !created.isEmpty()) meta.insert(QStringLiteral("created"), created);
    // The container's timecode (QuickTime tmcd, MXF), as written and in frames.
    QString timecode = tagOf(format->metadata, "timecode");
    for (unsigned i = 0; timecode.isEmpty() && i < format->nb_streams; ++i) timecode = tagOf(format->streams[i]->metadata, "timecode");
    if (!timecode.isEmpty()) {
        meta.insert(QStringLiteral("timecode"), timecode);
        const QStringList bits = timecode.split(QRegularExpression(QStringLiteral("[:;]")));
        if (bits.size() == 4) {
            const double rate = fps > 0 ? fps : 25.0;
            const qint64 seconds = bits.at(0).toInt() * 3600 + bits.at(1).toInt() * 60 + bits.at(2).toInt();
            meta.insert(QStringLiteral("timecodeFrames"), qint64(qRound64(seconds * rate)) + bits.at(3).toInt());
        }
    }
    for (const char *key : {"make", "com.apple.quicktime.make", "com.android.manufacturer"})
        if (const QString v = tagOf(format->metadata, key); !v.isEmpty()) { meta.insert(QStringLiteral("make"), v); break; }
    for (const char *key : {"model", "com.apple.quicktime.model", "com.android.model"})
        if (const QString v = tagOf(format->metadata, key); !v.isEmpty()) { meta.insert(QStringLiteral("model"), v); break; }
    // Every tag, verbatim: the container's by name, a stream's as "stream N name".
    QVariantMap tags;
    const AVDictionaryEntry *e = nullptr;
    while ((e = av_dict_iterate(format->metadata, e)))
        if (const QString v = QString::fromUtf8(e->value).trimmed(); !v.isEmpty()) tags.insert(QString::fromUtf8(e->key), v.left(2000));
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        e = nullptr;
        while ((e = av_dict_iterate(format->streams[i]->metadata, e)))
            if (const QString v = QString::fromUtf8(e->value).trimmed(); !v.isEmpty())
                tags.insert(QStringLiteral("stream %1 %2").arg(i).arg(QString::fromUtf8(e->key)), v.left(2000));
    }
    if (!tags.isEmpty()) meta.insert(QStringLiteral("containerTags"), tags);
    avformat_close_input(&format);
    return p;
}

Probe probeCached(const QString &path) {
    struct Known { qint64 size; QDateTime modified; Probe probe; };
    static QMutex mutex;
    static QHash<QString, Known> known;
    const QFileInfo info(path);
    {
        QMutexLocker lock(&mutex);
        const auto it = known.constFind(path);
        if (it != known.constEnd() && it->size == info.size() && it->modified == info.lastModified()) return it->probe;
    }
    Probe p = probe(path);
    QMutexLocker lock(&mutex);
    if (known.size() > 1024) known.clear();
    known.insert(path, {info.size(), info.lastModified(), p});
    return p;
}

bool soundOnly(const QString &path) {
    // Known for the file as it is (its size and date, as probeCached keeps
    // its answers): a file made again at the same path, a sound bounced again
    // with a picture, is asked about afresh.
    struct Known { qint64 size; QDateTime modified; bool only; };
    static QMutex mutex;
    static QHash<QString, Known> known;
    const QFileInfo info(path);
    {
        QMutexLocker lock(&mutex);
        const auto it = known.constFind(path);
        if (it != known.constEnd() && it->size == info.size() && it->modified == info.lastModified()) return it->only;
    }
    bool only = false;
    if (info.isFile()) {
        const Probe p = probeCached(path);
        only = p.valid && p.hasAudio && !p.hasVideo && !p.still;
    }
    QMutexLocker lock(&mutex);
    if (known.size() > 4096) known.clear();
    known.insert(path, {info.size(), info.lastModified(), only});
    return only;
}

int framesOn(const Probe &p, double fps) {
    if (!p.valid || fps <= 0) return 0;
    if (p.animationMilliseconds > 0) return std::max(1, int(std::ceil(p.animationMilliseconds * fps / 1000.0)));
    if (p.still) return kStillFrames;
    if (p.duration < 0) return INT_MAX;  // a live source runs on
    return int(std::lrint(double(p.duration) * fps / AV_TIME_BASE));
}

}  // namespace media
