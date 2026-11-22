#include "sink.h"
#include "stagetimer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" {
#include <libavutil/parseutils.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace deliver {

namespace {

QString averror(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return QString::fromUtf8(text);
}

// The swscale flags for a conversion.
int swsFlags(AVPixelFormat src, AVPixelFormat dst) {
    int flags = SWS_BICUBIC | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND;
    const AVPixFmtDescriptor *s = av_pix_fmt_desc_get(src), *d = av_pix_fmt_desc_get(dst);
    if (!s || !d) return flags;
    const bool srcRgb = s->flags & AV_PIX_FMT_FLAG_RGB, dstRgb = d->flags & AV_PIX_FMT_FLAG_RGB;
    if (srcRgb && !dstRgb) return SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (!srcRgb && dstRgb) return SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (!srcRgb && !dstRgb)
        return s->log2_chroma_w == d->log2_chroma_w && s->log2_chroma_h == d->log2_chroma_h ? SWS_POINT | SWS_ACCURATE_RND
                                                                                          : SWS_BILINEAR | SWS_ACCURATE_RND;
    return flags;
}

// The coefficients for an ITU matrix number.
const int *coefficients(int colourspace) {
    switch (colourspace) {
    case 601: return sws_getCoefficients(SWS_CS_ITU601);
    case 240: return sws_getCoefficients(SWS_CS_SMPTE240M);
    case 709: return sws_getCoefficients(SWS_CS_ITU709);
    case 2020: return sws_getCoefficients(SWS_CS_BT2020);
    default: return sws_getCoefficients(SWS_CS_DEFAULT);
    }
}

// x265's mastering display, G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min): chromaticity
// in 1/50000, luminance in 1/10000 cd/m².
bool masteringDisplay(const QString &text, AVMasteringDisplayMetadata *md) {
    int v[10];
    // The count of fields read is the check here.
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
    if (std::sscanf(text.toLatin1().constData(), "G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)", &v[0], &v[1], &v[2], &v[3],
                    &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) != 10)
        return false;
    const auto chroma = [](int n) { return av_make_q(n, 50000); };
    md->display_primaries[0][0] = chroma(v[4]);  // red
    md->display_primaries[0][1] = chroma(v[5]);
    md->display_primaries[1][0] = chroma(v[0]);  // green
    md->display_primaries[1][1] = chroma(v[1]);
    md->display_primaries[2][0] = chroma(v[2]);  // blue
    md->display_primaries[2][1] = chroma(v[3]);
    md->white_point[0] = chroma(v[6]);
    md->white_point[1] = chroma(v[7]);
    md->max_luminance = av_make_q(v[8], 10000);
    md->min_luminance = av_make_q(v[9], 10000);
    md->has_primaries = md->has_luminance = 1;
    return true;
}

// A tag by its FFmpeg name; unspecified for a name it does not know.
AVColorPrimaries primariesNamed(const QString &name) {
    const int v = av_color_primaries_from_name(name.toUtf8().constData());
    return v < 0 ? AVCOL_PRI_UNSPECIFIED : AVColorPrimaries(v);
}
AVColorTransferCharacteristic transferNamed(const QString &name) {
    const int v = av_color_transfer_from_name(name.toUtf8().constData());
    return v < 0 ? AVCOL_TRC_UNSPECIFIED : AVColorTransferCharacteristic(v);
}
AVColorSpace matrixNamed(const QString &name) {
    const int v = av_color_space_from_name(name.toUtf8().constData());
    return v < 0 ? AVCOL_SPC_UNSPECIFIED : AVColorSpace(v);
}

}  // namespace

struct Sink::State {
    AVFormatContext *format = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVStream *videoStream = nullptr, *audioStream = nullptr;
    SwsContext *scale = nullptr;
    SwrContext *resample = nullptr;
    AVAudioFifo *fifo = nullptr;
    AVFrame *picture = nullptr, *sound = nullptr;
    AVBufferRef *device = nullptr;  // a hardware encoder's device, when it has one
    AVFrame *uploaded = nullptr;    // the picture on that device
    AVPacket *packet = nullptr;
    ExportSpec::EncoderSettings settings;
    int width = 0, height = 0, pictureColourspace = 709;
    // The tags each picture carries: the container's, or the encoder header's own set.
    AVColorPrimaries picturePrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic pictureTrc = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace pictureMatrix = AVCOL_SPC_UNSPECIFIED;
    qint64 pictures = 0, samples = 0;
    bool headerWritten = false, opened = false;
    // A command-line encoder (settings.pipeEncoder): the pictures go to it raw,
    // the sound waits in a file of its own, and finish() puts both in the
    // container.
    QString path;
    AVRational rate{25, 1};
    std::unique_ptr<QProcess> encoder;
    QString stream;             // the raw stream it writes
    std::unique_ptr<QFile> held;  // the sound meanwhile: interleaved float

    ~State() {
        sws_freeContext(scale);
        swr_free(&resample);
        if (fifo) av_audio_fifo_free(fifo);
        av_frame_free(&picture);
        av_frame_free(&uploaded);
        av_frame_free(&sound);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        av_buffer_unref(&device);
        if (format) {
            if (opened && !(format->oformat->flags & AVFMT_NOFILE)) avio_closep(&format->pb);
            avformat_free_context(format);
        }
        if (encoder) {
            encoder->kill();
            encoder->waitForFinished(5000);
        }
        if (!stream.isEmpty()) QFile::remove(stream);
        if (held) held->remove();
    }

    bool openContainer(AVStream *copied, QString *error);
    bool preparePictures(AVPixelFormat format, QString *error);
    bool submitPicture(QString *error);
    bool finishPipe(QString *error);
    bool writeSoundNow(const float *samples, int count, QString *error);

    // Everything an encoder has ready, into the muxer.
    bool drain(AVCodecContext *codec, AVStream *stream, QString *error) {
        for (;;) {
            const int got = avcodec_receive_packet(codec, packet);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
            if (got < 0) { *error = QStringLiteral("Encoding failed: ") + averror(got); return false; }
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int written = av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
            if (written < 0) { *error = QStringLiteral("Writing the file failed: ") + averror(written); return false; }
        }
    }
    bool send(AVCodecContext *codec, AVStream *stream, AVFrame *frame, QString *error) {
        const int sent = avcodec_send_frame(codec, frame);
        if (sent < 0 && sent != AVERROR_EOF) { *error = QStringLiteral("Encoding failed: ") + averror(sent); return false; }
        return drain(codec, stream, error);
    }
    // Sound from the FIFO in the encoder's frame size (all of it at the end).
    bool encodeSound(bool final, QString *error) {
        // An encoder that takes any size (PCM) gets 20 ms packets: a whole
        // number of milliseconds, so a millisecond container clock (Matroska)
        // stamps every one exactly and a reader seeking in it finds no gaps.
        const int frameSize = audio->frame_size > 0 && !(audio->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
                                  ? audio->frame_size
                                  : audio->sample_rate % 50 == 0 ? audio->sample_rate / 50 : 1024;
        while (av_audio_fifo_size(fifo) >= frameSize || (final && av_audio_fifo_size(fifo) > 0)) {
            const int available = av_audio_fifo_size(fifo);
            int n = std::min(frameSize, available);
            av_frame_unref(sound);
            sound->format = audio->sample_fmt;
            av_channel_layout_copy(&sound->ch_layout, &audio->ch_layout);
            sound->sample_rate = audio->sample_rate;
            // A fixed-size encoder's last frame is padded with silence.
            const bool pad = n < frameSize && audio->frame_size > 0
                             && !(audio->codec->capabilities & (AV_CODEC_CAP_VARIABLE_FRAME_SIZE | AV_CODEC_CAP_SMALL_LAST_FRAME));
            sound->nb_samples = pad ? frameSize : n;
            if (av_frame_get_buffer(sound, 0) < 0) { *error = QStringLiteral("Out of memory for sound."); return false; }
            if (pad) av_samples_set_silence(sound->data, 0, frameSize, audio->ch_layout.nb_channels, audio->sample_fmt);
            if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(sound->data), n) != n) { *error = QStringLiteral("The sound buffer failed."); return false; }
            sound->pts = samples;
            samples += sound->nb_samples;
            if (!send(audio, audioStream, sound, error)) return false;
        }
        return true;
    }
};

Sink::Sink() : d(std::make_unique<State>()) {}
Sink::~Sink() = default;

qint64 Sink::pictures() const { return d->pictures; }

// The next file of an image sequence, written under a name of its own and
// moved into place whole: a file that is there is a finished picture.
static bool writeSequenceFile(const QString &folder, const ExportSpec::EncoderSettings &settings, qint64 written,
                              const std::function<bool(const QString &, QString *)> &write, QString *error) {
    const QString name = ExportSpec::sequenceFileName(settings.sequenceStem, settings.sequenceFirst + int(written), settings.sequence);
    const QString file = folder + QLatin1Char('/') + name, pending = folder + QStringLiteral("/.writing-") + name;
    QString why;
    if (!write(pending, &why) || std::rename(QFile::encodeName(pending).constData(), QFile::encodeName(file).constData()) != 0) {
        QFile::remove(pending);
        *error = QStringLiteral("The picture %1 cannot be written%2").arg(name, why.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + why);
        return false;
    }
    return true;
}

bool Sink::wantsLinear() const { return d->settings.sequence == QLatin1String("exr"); }

bool Sink::writeLinear(const WorkingFrame &frame, QString *error) {
    auto &s = *d;
    if (!wantsLinear() || frame.isNull() || frame.pixels().width() != s.width || frame.pixels().height() != s.height) {
        *error = QStringLiteral("A picture of the wrong shape reached the image sequence.");
        return false;
    }
    if (!writeSequenceFile(s.path, s.settings, s.pictures, [&](const QString &to, QString *why) { return frame.save(to, {}, why); }, error))
        return false;
    ++s.pictures;
    return true;
}

bool Sink::open(const QString &path, const ExportSpec::EncoderSettings &settings, int width, int height, int fpsNum, int fpsDen,
                int pictureColourspace, QString *error) {
    auto &s = *d;
    s.settings = settings;
    s.width = settings.width > 0 ? settings.width : width;
    s.height = settings.height > 0 ? settings.height : height;
    if (!settings.sequence.isEmpty()) {
        // A folder of numbered files; one already there (a paused render's) is added to.
        s.path = path;
        if (!settings.video || !QDir().mkpath(path)) { *error = QStringLiteral("The image sequence's folder cannot be made."); return false; }
        return true;
    }
    s.pictureColourspace = pictureColourspace;
    s.path = path;
    s.rate = AVRational{fpsNum, fpsDen};
    s.packet = av_packet_alloc();
    if (!settings.video || settings.pipeEncoder.isEmpty()) {
        if (settings.video && !settings.pipeStream.isEmpty()) {
            *error = QStringLiteral("The %1 encoder is not installed.").arg(settings.vcodec);
            return false;
        }
        return s.openContainer(nullptr, error);
    }
    // The command-line encoder, fed raw pictures as they come.
    const AVPixelFormat raw = av_get_pix_fmt(settings.pixFmt.toUtf8().constData());
    if (raw == AV_PIX_FMT_NONE) { *error = QStringLiteral("Unknown pixel format %1.").arg(settings.pixFmt); return false; }
    if (!s.preparePictures(raw, error)) return false;
    s.picturePrimaries = primariesNamed(settings.primaries);
    s.pictureTrc = transferNamed(settings.trc);
    s.pictureMatrix = matrixNamed(settings.matrix);
    s.stream = path + QStringLiteral(".") + settings.pipeStream;
    QFile::remove(s.stream);
    if (settings.audio) {
        s.held = std::make_unique<QFile>(path + QStringLiteral(".sound"));
        if (!s.held->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
            *error = QStringLiteral("The sound cannot be held: ") + s.held->errorString();
            return false;
        }
    }
    s.encoder = std::make_unique<QProcess>();
    s.encoder->setStandardOutputFile(QProcess::nullDevice());
    QStringList args = settings.pipeEncoder.mid(1);
    args << QStringLiteral("-i") << QStringLiteral("/dev/stdin") << QStringLiteral("-w") << QString::number(s.width)
         << QStringLiteral("-h") << QString::number(s.height) << QStringLiteral("-o") << s.stream << QStringLiteral("-v")
         << QStringLiteral("0");
    s.encoder->start(settings.pipeEncoder.first(), args);
    if (!s.encoder->waitForStarted(10000)) {
        *error = QStringLiteral("The %1 encoder would not start: %2").arg(settings.vcodec, s.encoder->errorString());
        return false;
    }
    return true;
}

// Pictures converted with those swscale flags and
// coefficients, a full-range source and the delivery's range.
bool Sink::State::preparePictures(AVPixelFormat converted, QString *error) {
    picture = av_frame_alloc();
    picture->format = converted;
    picture->width = width;
    picture->height = height;
    if (av_frame_get_buffer(picture, 0) < 0) { *error = QStringLiteral("Out of memory for pictures."); return false; }
    scale = sws_getContext(width, height, AV_PIX_FMT_RGBA64LE, width, height, converted, swsFlags(AV_PIX_FMT_RGBA64LE, converted),
                           nullptr, nullptr, nullptr);
    if (!scale) { *error = QStringLiteral("The pictures cannot be converted to %1.").arg(av_get_pix_fmt_name(converted)); return false; }
    sws_setColorspaceDetails(scale, coefficients(pictureColourspace), 1, coefficients(settings.colorspace),
                             settings.range == QLatin1String("pc") ? 1 : 0, 0, 1 << 16, 1 << 16);
    return true;
}

// The container, its encoders and its header. `copied`: a finished picture
// stream to carry as it is (the command-line encoder's) instead of encoding.
bool Sink::State::openContainer(AVStream *copied, QString *error) {
    const QByteArray file = QFile::encodeName(path), muxer = settings.muxer.toUtf8();
    if (avformat_alloc_output_context2(&format, nullptr, muxer.isEmpty() ? nullptr : muxer.constData(), file.constData()) < 0 || !format) {
        *error = QStringLiteral("The container %1 cannot be written.").arg(settings.muxer);
        return false;
    }
    const AVRational frameTime{rate.den, rate.num};
    // The spec's options are AVOptions but for two names the sink reads
    // itself: a fixed quantiser ("qscale") and the sound's bit rate ("ab").
    const auto options = [](const QList<QPair<QString, QString>> &list, AVCodecContext *codec) {
        AVDictionary *dict = nullptr;
        for (const auto &kv : list) {
            if (kv.first == QLatin1String("qscale")) {
                codec->flags |= AV_CODEC_FLAG_QSCALE;
                codec->global_quality = int(FF_QP2LAMBDA * kv.second.toDouble());
                continue;
            }
            const QByteArray name = kv.first == QLatin1String("ab") ? QByteArray("b") : kv.first.toUtf8();
            av_dict_set(&dict, name.constData(), kv.second.toUtf8().constData(), 0);
        }
        return dict;
    };
    if (settings.video && !copied) {
        const AVCodec *codec = avcodec_find_encoder_by_name(settings.vcodec.toUtf8().constData());
        if (!codec) { *error = QStringLiteral("The encoder %1 is not available.").arg(settings.vcodec); return false; }
        video = avcodec_alloc_context3(codec);
        video->width = width;
        video->height = height;
        video->time_base = frameTime;
        video->framerate = rate;
        // The pictures as the CPU converts them: the encoder's own format, or
        // what a hardware encoder's device takes them in.
        const QString cpuFormat = settings.hw.isEmpty() ? settings.pixFmt : settings.hwFormat;
        const AVPixelFormat converted = av_get_pix_fmt(cpuFormat.toUtf8().constData());
        if (converted == AV_PIX_FMT_NONE) { *error = QStringLiteral("Unknown pixel format %1.").arg(cpuFormat); return false; }
        video->pix_fmt = converted;
        if (!settings.hw.isEmpty()) {
            const AVHWDeviceType type = av_hwdevice_find_type_by_name(settings.hw.toUtf8().constData());
            const QByteArray node = settings.hwDevice.toUtf8();
            const int made = type == AV_HWDEVICE_TYPE_NONE ? AVERROR(ENOSYS)
                           : av_hwdevice_ctx_create(&device, type, node.isEmpty() ? nullptr : node.constData(), nullptr, 0);
            if (made < 0) {
                *error = QStringLiteral("The %1 device would not open: %2").arg(settings.hw, averror(made));
                return false;
            }
            AVBufferRef *frames = av_hwframe_ctx_alloc(device);
            auto *pool = frames ? reinterpret_cast<AVHWFramesContext *>(frames->data) : nullptr;
            if (pool) {
                pool->format = type == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_VULKAN;
                pool->sw_format = converted;
                pool->width = width;
                pool->height = height;
            }
            const int ready = pool ? av_hwframe_ctx_init(frames) : AVERROR(ENOMEM);
            if (ready < 0) {
                av_buffer_unref(&frames);
                *error = QStringLiteral("The %1 device cannot take %2 pictures: %3").arg(settings.hw, cpuFormat, averror(ready));
                return false;
            }
            video->pix_fmt = pool->format;
            video->hw_frames_ctx = frames;  // the context's own reference
            uploaded = av_frame_alloc();
        }
        if (settings.sarNum > 0) video->sample_aspect_ratio = av_d2q(double(settings.sarNum) / settings.sarDen, 255);
        video->color_primaries = primariesNamed(settings.primaries);
        video->color_trc = transferNamed(settings.trc);
        video->colorspace = matrixNamed(settings.matrix);
        picturePrimaries = settings.headerPrimaries.isEmpty() ? video->color_primaries : primariesNamed(settings.headerPrimaries);
        pictureTrc = settings.headerTrc.isEmpty() ? video->color_trc : transferNamed(settings.headerTrc);
        pictureMatrix = settings.headerMatrix.isEmpty() ? video->colorspace : matrixNamed(settings.headerMatrix);
        video->color_range = settings.range == QLatin1String("pc") ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        if (!settings.progressive && settings.hw != QLatin1String("vaapi")) {
            video->field_order = settings.topFieldFirst ? AV_FIELD_TT : AV_FIELD_BB;
            video->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
        }
        video->thread_count = 0;
        if (format->oformat->flags & AVFMT_GLOBALHEADER) video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *dict = options(settings.videoOptions, video);
        const int opened = avcodec_open2(video, codec, &dict);
        av_dict_free(&dict);
        if (opened < 0) { *error = QStringLiteral("The encoder %1 would not open: %2").arg(settings.vcodec, averror(opened)); return false; }
        if (!preparePictures(converted, error)) return false;
    }
    if (settings.video) {
        videoStream = avformat_new_stream(format, nullptr);
        videoStream->time_base = frameTime;
        videoStream->avg_frame_rate = rate;
        AVCodecParameters *par = videoStream->codecpar;
        if (copied) {
            avcodec_parameters_copy(par, copied->codecpar);
            par->codec_tag = 0;
            par->color_primaries = primariesNamed(settings.primaries);
            par->color_trc = transferNamed(settings.trc);
            par->color_space = matrixNamed(settings.matrix);
            par->color_range = settings.range == QLatin1String("pc") ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            if (settings.sarNum > 0) par->sample_aspect_ratio = av_d2q(double(settings.sarNum) / settings.sarDen, 255);
        } else {
            avcodec_parameters_from_context(par, video);
        }
        videoStream->sample_aspect_ratio = par->sample_aspect_ratio;
        if (!settings.masterDisplay.isEmpty()) {
            AVMasteringDisplayMetadata md{};
            if (masteringDisplay(settings.masterDisplay, &md))
                if (AVPacketSideData *sd = av_packet_side_data_new(&par->coded_side_data, &par->nb_coded_side_data,
                                                                   AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof md, 0))
                    std::memcpy(sd->data, &md, sizeof md);
        }
        if (settings.maxCll > 0 || settings.maxFall > 0) {
            const AVContentLightMetadata light{unsigned(settings.maxCll), unsigned(settings.maxFall)};
            if (AVPacketSideData *sd = av_packet_side_data_new(&par->coded_side_data, &par->nb_coded_side_data,
                                                               AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof light, 0))
                std::memcpy(sd->data, &light, sizeof light);
        }
    }
    if (settings.audio) {
        const AVCodec *codec = avcodec_find_encoder_by_name(settings.acodec.toUtf8().constData());
        if (!codec) { *error = QStringLiteral("The sound encoder %1 is not available.").arg(settings.acodec); return false; }
        audio = avcodec_alloc_context3(codec);
        audio->sample_rate = settings.arate;
        av_channel_layout_default(&audio->ch_layout, settings.channels);
        // The encoder's own sample format: float when it takes it; failing
        // that its widest whole-number one, so a lossless codec keeps 24 bits
        // of the mix and not the 16 of the first format it lists (FLAC); else
        // its first.
        const AVSampleFormat *formats = nullptr;
        int count = 0;
        avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, reinterpret_cast<const void **>(&formats), &count);
        audio->sample_fmt = formats && count > 0 ? formats[0] : AV_SAMPLE_FMT_FLTP;
        bool wide = false;
        for (int i = 0; formats && i < count; ++i) {
            if (formats[i] == AV_SAMPLE_FMT_FLTP || formats[i] == AV_SAMPLE_FMT_FLT) { audio->sample_fmt = formats[i]; break; }
            if (!wide && (formats[i] == AV_SAMPLE_FMT_S32 || formats[i] == AV_SAMPLE_FMT_S32P)) { audio->sample_fmt = formats[i]; wide = true; }
        }
        audio->time_base = AVRational{1, settings.arate};
        if (format->oformat->flags & AVFMT_GLOBALHEADER) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *dict = options(settings.audioOptions, audio);
        const int opened = avcodec_open2(audio, codec, &dict);
        av_dict_free(&dict);
        if (opened < 0) { *error = QStringLiteral("The sound encoder %1 would not open: %2").arg(settings.acodec, averror(opened)); return false; }
        audioStream = avformat_new_stream(format, nullptr);
        audioStream->time_base = audio->time_base;
        avcodec_parameters_from_context(audioStream->codecpar, audio);
        AVChannelLayout in{};
        av_channel_layout_default(&in, settings.channels);
        if (swr_alloc_set_opts2(&resample, &audio->ch_layout, audio->sample_fmt, settings.arate, &in, AV_SAMPLE_FMT_FLT,
                                settings.arate, 0, nullptr) < 0 || swr_init(resample) < 0) {
            *error = QStringLiteral("The sound cannot be converted for %1.").arg(settings.acodec);
            return false;
        }
        fifo = av_audio_fifo_alloc(audio->sample_fmt, settings.channels, 4096);
        sound = av_frame_alloc();
    }
    for (const auto &kv : settings.metadata) av_dict_set(&format->metadata, kv.first.toUtf8().constData(), kv.second.toUtf8().constData(), 0);
    // Where the file starts on the timeline's clock: the muxer makes a
    // timecode track (QuickTime, MP4) or a tag (Matroska, WebM) of the one,
    // a broadcast-wave chunk of the other.
    if (!settings.timecode.isEmpty()) av_dict_set(&format->metadata, "timecode", settings.timecode.toLatin1().constData(), 0);
    if (settings.timeReference >= 0) av_dict_set(&format->metadata, "time_reference", QByteArray::number(settings.timeReference).constData(), 0);
    for (const ExportSpec::Chapter &from : settings.chapters) {
        auto *chapter = static_cast<AVChapter *>(av_mallocz(sizeof(AVChapter)));
        auto **list = chapter ? static_cast<AVChapter **>(av_realloc_array(static_cast<void *>(format->chapters), format->nb_chapters + 1, sizeof(AVChapter *))) : nullptr;
        if (!list) { av_free(chapter); *error = QStringLiteral("The chapters cannot be written: out of memory."); return false; }
        format->chapters = list;
        chapter->id = format->nb_chapters + 1;
        chapter->time_base = AVRational{1, 1000};
        chapter->start = from.startMs;
        chapter->end = from.endMs;
        av_dict_set(&chapter->metadata, "title", from.title.toUtf8().constData(), 0);
        format->chapters[format->nb_chapters++] = chapter;
    }
    if (!(format->oformat->flags & AVFMT_NOFILE)) {
        const int io = avio_open(&format->pb, file.constData(), AVIO_FLAG_WRITE);
        if (io < 0) { *error = QStringLiteral("The file cannot be written: ") + averror(io); return false; }
        opened = true;
    }
    AVDictionary *header = nullptr;
    if (!settings.movflags.isEmpty()) av_dict_set(&header, "movflags", settings.movflags.toUtf8().constData(), 0);
    // An MP4 holds a timecode track only when asked, a WAV its broadcast chunk likewise.
    if (!settings.timecode.isEmpty() && settings.muxer == QLatin1String("mp4")) av_dict_set(&header, "write_tmcd", "1", 0);
    if (settings.timeReference >= 0) av_dict_set(&header, "write_bext", "1", 0);
    const int written = avformat_write_header(format, &header);
    av_dict_free(&header);
    if (written < 0) { *error = QStringLiteral("The file header cannot be written: ") + averror(written); return false; }
    headerWritten = true;
    return true;
}

bool Sink::writePicture(const QImage &image, QString *error) {
    auto &s = *d;
    if (!s.settings.sequence.isEmpty()) {
        if (image.width() != s.width || image.height() != s.height || wantsLinear()) {
            *error = QStringLiteral("A picture of the wrong shape reached the image sequence.");
            return false;
        }
        const QString &kind = s.settings.sequence;
        const bool alpha = s.settings.sequenceAlpha && kind != QLatin1String("jpeg");
        // 16 bits as they came; 8 bits rounded by Qt's own conversion.
        const QImage::Format format = kind == QLatin1String("png16") || kind == QLatin1String("tiff16")
                                          ? (alpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64)
                                          : alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
        const QImage packed = image.format() == format ? image : image.convertToFormat(format);
        const auto write = [&](const QString &to, QString *why) {
            QImageWriter writer(to, kind.startsWith(QLatin1String("png")) ? "png" : kind == QLatin1String("tiff16") ? "tiff" : "jpeg");
            if (kind == QLatin1String("jpeg")) writer.setQuality(95);
            if (kind == QLatin1String("tiff16")) writer.setCompression(1);  // LZW: lossless
            if (writer.write(packed)) return true;
            *why = writer.errorString();
            return false;
        };
        if (!writeSequenceFile(s.path, s.settings, s.pictures, write, error)) return false;
        ++s.pictures;
        return true;
    }
    if (!s.picture) return true;
    const QImage source = image.format() == QImage::Format_RGBA64 ? image : image.convertToFormat(QImage::Format_RGBA64);
    if (source.width() != s.width || source.height() != s.height) {
        *error = QStringLiteral("A picture of the wrong size reached the encoder.");
        return false;
    }
    if (av_frame_make_writable(s.picture) < 0) { *error = QStringLiteral("Out of memory for pictures."); return false; }
    const uint8_t *planes[1] = {source.constBits()};
    const int strides[1] = {int(source.bytesPerLine())};
    {
        StageTimer::Scope timed("sink.convert");
        sws_scale(s.scale, planes, strides, 0, s.height, s.picture->data, s.picture->linesize);
    }
    return s.submitPicture(error);
}

namespace {
// The engine's layout for an encoder's picture format, when it has one.
std::optional<engine::ColourFormat> engineLayout(int format) {
    switch (format) {
    case AV_PIX_FMT_NV12: return engine::ColourFormat::NV12;
    case AV_PIX_FMT_YUV420P: return engine::ColourFormat::I420;
    case AV_PIX_FMT_YUV444P: return engine::ColourFormat::I444;
    case AV_PIX_FMT_P010LE: return engine::ColourFormat::P010;
    case AV_PIX_FMT_YUV420P10LE: return engine::ColourFormat::I010;
    case AV_PIX_FMT_YUV422P10LE: return engine::ColourFormat::I210;
    case AV_PIX_FMT_P216LE: return engine::ColourFormat::P216;
    case AV_PIX_FMT_P416LE: return engine::ColourFormat::P416;
    default: return std::nullopt;
    }
}
}  // namespace

std::optional<Sink::Planes> Sink::planes() const {
    const auto &s = *d;
    if (!s.picture) return std::nullopt;
    const auto layout = engineLayout(s.picture->format);
    if (!layout) return std::nullopt;
    return Planes{*layout, s.settings.colorspace, s.settings.range == QLatin1String("pc")};
}

bool Sink::writePlanes(const engine::YuvFrame &frame, QString *error) {
    auto &s = *d;
    if (!s.picture) return true;
    const auto layout = engineLayout(s.picture->format);
    if (!layout || frame.format != *layout || frame.width != s.width || frame.height != s.height) {
        *error = QStringLiteral("A picture of the wrong shape reached the encoder.");
        return false;
    }
    if (av_frame_make_writable(s.picture) < 0) { *error = QStringLiteral("Out of memory for pictures."); return false; }
    {
        StageTimer::Scope timed("sink.planes");
        const std::vector<uint8_t> *from[3] = {&frame.y, &frame.u, &frame.v};
        for (int plane = 0; plane < 3; ++plane) {
            const int bytes = frame.rowBytes(plane), rows = frame.rows(plane);
            for (int y = 0; y < rows; ++y)
                std::memcpy(s.picture->data[plane] + qsizetype(y) * s.picture->linesize[plane],
                            from[plane]->data() + qsizetype(y) * bytes, size_t(bytes));
        }
    }
    return s.submitPicture(error);
}

// The picture in s.picture, stamped, to the encoder.
bool Sink::State::submitPicture(QString *error) {
    auto &s = *this;
    s.picture->pts = s.pictures++;
    if (s.encoder) {
        // Raw planes, row by row, as the encoder reads them.
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(s.picture->format));
        const int bytes = (desc->comp[0].depth + 7) / 8;
        for (int plane = 0; plane < av_pix_fmt_count_planes(AVPixelFormat(s.picture->format)); ++plane) {
            const bool chroma = plane == 1 || plane == 2;
            const int w = chroma ? AV_CEIL_RSHIFT(s.width, desc->log2_chroma_w) : s.width;
            const int h = chroma ? AV_CEIL_RSHIFT(s.height, desc->log2_chroma_h) : s.height;
            for (int y = 0; y < h; ++y) {
                const char *row = reinterpret_cast<const char *>(s.picture->data[plane] + qsizetype(y) * s.picture->linesize[plane]);
                if (s.encoder->write(row, qint64(w) * bytes) != qint64(w) * bytes) {
                    *error = QStringLiteral("The %1 encoder stopped taking pictures.").arg(s.settings.vcodec);
                    return false;
                }
            }
        }
        // Keep at most a few pictures in flight.
        while (s.encoder->bytesToWrite() > qint64(s.width) * s.height * 16)
            if (!s.encoder->waitForBytesWritten(30000)) {
                *error = QStringLiteral("The %1 encoder stopped taking pictures: %2").arg(s.settings.vcodec, s.encoder->errorString());
                return false;
            }
        return true;
    }
    // A fixed quantiser is read from each picture.
    if (s.video->flags & AV_CODEC_FLAG_QSCALE) s.picture->quality = s.video->global_quality;
    s.picture->color_primaries = s.picturePrimaries;
    s.picture->color_trc = s.pictureTrc;
    s.picture->colorspace = s.pictureMatrix;
    s.picture->color_range = s.video->color_range;
    s.picture->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    if (!s.settings.progressive && s.video->field_order != AV_FIELD_UNKNOWN) {
        s.picture->flags |= AV_FRAME_FLAG_INTERLACED;
        if (s.settings.topFieldFirst) s.picture->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
    StageTimer::Scope timed("sink.encode");
    if (!s.uploaded) return s.send(s.video, s.videoStream, s.picture, error);
    // Onto the hardware encoder's device.
    av_frame_unref(s.uploaded);
    int up = av_hwframe_get_buffer(s.video->hw_frames_ctx, s.uploaded, 0);
    if (up >= 0) up = av_hwframe_transfer_data(s.uploaded, s.picture, 0);
    if (up >= 0) up = av_frame_copy_props(s.uploaded, s.picture);
    if (up < 0) { *error = QStringLiteral("A picture could not reach the %1 encoder: %2").arg(s.settings.hw, averror(up)); return false; }
    return s.send(s.video, s.videoStream, s.uploaded, error);
}

bool Sink::writeSound(const float *samples, int count, QString *error) {
    auto &s = *d;
    if (count <= 0) return true;
    if (s.held) {
        const qint64 bytes = qint64(count) * s.settings.channels * qint64(sizeof(float));
        if (s.held->write(reinterpret_cast<const char *>(samples), bytes) != bytes) {
            *error = QStringLiteral("The sound cannot be held: ") + s.held->errorString();
            return false;
        }
        return true;
    }
    return s.writeSoundNow(samples, count, error);
}

// Sound into its encoder: converted, then encoded a codec frame at a time.
bool Sink::State::writeSoundNow(const float *samples, int count, QString *error) {
    if (!audio || count <= 0) return true;
    uint8_t **converted = nullptr;
    if (av_samples_alloc_array_and_samples(&converted, nullptr, settings.channels, count, audio->sample_fmt, 0) < 0) {
        *error = QStringLiteral("Out of memory for sound.");
        return false;
    }
    const uint8_t *in[1] = {reinterpret_cast<const uint8_t *>(samples)};
    const int made = swr_convert(resample, converted, count, in, count);
    bool ok = made >= 0 && av_audio_fifo_write(fifo, reinterpret_cast<void **>(converted), made) == made;
    av_freep(static_cast<void *>(&converted[0]));
    av_freep(static_cast<void *>(&converted));
    if (!ok) { *error = QStringLiteral("The sound could not be converted."); return false; }
    return encodeSound(false, error);
}

// The command-line encoder's stream and the held sound, into the container:
// the stream as it is, the sound encoded beside it.
bool Sink::State::finishPipe(QString *error) {
    // Every picture through the pipe first: this thread has no event loop to
    // flush it after the channel closes.
    while (encoder->bytesToWrite() > 0)
        if (!encoder->waitForBytesWritten(30000)) {
            *error = QStringLiteral("The %1 encoder stopped taking pictures: %2").arg(settings.vcodec, encoder->errorString());
            return false;
        }
    encoder->closeWriteChannel();
    const bool ended = encoder->waitForFinished(-1);
    const QString said = QString::fromUtf8(encoder->readAllStandardError()).trimmed();
    if (!ended || encoder->exitStatus() != QProcess::NormalExit || encoder->exitCode() != 0) {
        *error = QStringLiteral("The %1 encoder failed").arg(settings.vcodec) + (said.isEmpty() ? QString() : QStringLiteral(": ") + said);
        return false;
    }
    encoder.reset();
    AVFormatContext *input = nullptr;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "framerate", QStringLiteral("%1/%2").arg(rate.num).arg(rate.den).toLatin1().constData(), 0);
    const AVInputFormat *raw = av_find_input_format(settings.pipeStream.toUtf8().constData());
    int got = avformat_open_input(&input, QFile::encodeName(stream).constData(), raw, &options);
    av_dict_free(&options);
    if (got >= 0) got = avformat_find_stream_info(input, nullptr);
    if (got < 0 || input->nb_streams < 1) {
        if (input) avformat_close_input(&input);
        *error = QStringLiteral("The %1 stream cannot be read: %2").arg(settings.vcodec, averror(got < 0 ? got : AVERROR_INVALIDDATA));
        return false;
    }
    const auto close = qScopeGuard([&] { avformat_close_input(&input); });
    if (!openContainer(input->streams[0], error)) return false;
    std::vector<float> chunk;
    if (held) held->seek(0);
    qint64 heard = 0;
    // Each picture's own sound before it, so the muxer interleaves them.
    const auto soundUpTo = [&](qint64 frame) {
        if (!held) return true;
        const qint64 until = av_rescale(frame, qint64(settings.arate) * rate.den, rate.num);
        while (heard < until) {
            const int n = int(std::min<qint64>(until - heard, 4096));
            chunk.resize(size_t(n) * settings.channels);
            const qint64 bytes = qint64(chunk.size()) * qint64(sizeof(float));
            const qint64 read = held->read(reinterpret_cast<char *>(chunk.data()), bytes);
            if (read <= 0) { heard = until; break; }
            const int samplesRead = int(read / (qint64(sizeof(float)) * settings.channels));
            if (!writeSoundNow(chunk.data(), samplesRead, error)) return false;
            heard += samplesRead;
            if (read < bytes) { heard = until; break; }
        }
        return true;
    };
    // The stream's own packet: the sound encoder's is in use between reads.
    AVPacket *carried = av_packet_alloc();
    const auto release = qScopeGuard([&] { av_packet_free(&carried); });
    qint64 frame = 0;
    for (;;) {
        if (!soundUpTo(frame + 1)) return false;
        got = av_read_frame(input, carried);
        if (got == AVERROR_EOF) break;
        if (got < 0) { *error = QStringLiteral("The %1 stream cannot be read: %2").arg(settings.vcodec, averror(got)); return false; }
        carried->pts = carried->dts = frame++;
        carried->duration = 1;
        av_packet_rescale_ts(carried, AVRational{rate.den, rate.num}, videoStream->time_base);
        carried->stream_index = videoStream->index;
        carried->flags |= AV_PKT_FLAG_KEY;
        const int written = av_interleaved_write_frame(format, carried);
        av_packet_unref(carried);
        if (written < 0) { *error = QStringLiteral("Writing the file failed: ") + averror(written); return false; }
    }
    if (frame != pictures) {
        *error = QStringLiteral("The %1 encoder returned %2 of %3 pictures.").arg(settings.vcodec).arg(frame).arg(pictures);
        return false;
    }
    if (held) {
        // Whatever is left (the range's last samples).
        for (;;) {
            chunk.resize(size_t(4096) * settings.channels);
            const qint64 read = held->read(reinterpret_cast<char *>(chunk.data()), qint64(chunk.size()) * qint64(sizeof(float)));
            if (read <= 0) break;
            if (!writeSoundNow(chunk.data(), int(read / (qint64(sizeof(float)) * settings.channels)), error)) return false;
        }
        held->remove();
        held.reset();
    }
    QFile::remove(stream);
    stream.clear();
    return true;
}

bool Sink::finish(QString *error) {
    auto &s = *d;
    if (!s.settings.sequence.isEmpty()) return true;  // every file was finished as it was written
    if (s.encoder && !s.finishPipe(error)) return false;
    if (!s.headerWritten) { *error = QStringLiteral("The file was never opened."); return false; }
    if (s.audio && !s.encodeSound(true, error)) return false;
    if (s.video && !s.send(s.video, s.videoStream, nullptr, error)) return false;
    if (s.audio && !s.send(s.audio, s.audioStream, nullptr, error)) return false;
    const int trailer = av_write_trailer(s.format);
    if (trailer < 0) { *error = QStringLiteral("The file could not be finished: ") + averror(trailer); return false; }
    if (s.opened) avio_closep(&s.format->pb);
    s.opened = false;
    return true;
}

namespace {
bool answersProbe = false;
const char probeFlag[] = "--omaedit-encoder-probe";

// One picture and its share of sound through a sink opened as the delivery opens it.
bool probe(const QVariantMap &request, QString *error) {
    const QVariantMap spec = request.value("spec").toMap();
    const int width = request.value("width").toInt(), height = request.value("height").toInt();
    AVRational timeline{request.value("fpsNum").toInt(), request.value("fpsDen").toInt()};
    if (width <= 0 || height <= 0 || timeline.num <= 0 || timeline.den <= 0) {
        *error = QStringLiteral("The encoder check was not given the timeline's size and rate.");
        return false;
    }
    // A delivery at another frame rate is encoded at that rate.
    AVRational rate = timeline;
    const QString converted = ExportSpec::convertedRate(spec, av_q2d(timeline));
    if (!converted.isEmpty() && av_parse_video_rate(&rate, converted.toLatin1().constData()) < 0) {
        *error = QStringLiteral("The frame rate %1 cannot be delivered.").arg(converted);
        return false;
    }
    const ExportSpec::EncoderSettings settings = ExportSpec::encoderSettings(spec, av_q2d(rate), false);
    Sink sink;
    if (!sink.open(request.value("output").toString(), settings, width, height, rate.num, rate.den, request.value("colourspace", 709).toInt(), error))
        return false;
    if (settings.video) {
        QImage picture(settings.width > 0 ? settings.width : width, settings.height > 0 ? settings.height : height, QImage::Format_RGBA64);
        picture.fill(Qt::black);
        if (!sink.writePicture(picture, error)) return false;
    }
    if (settings.audio) {
        const int samples = std::max(1, int(av_rescale(settings.arate, rate.den, rate.num)));
        const std::vector<float> silence(size_t(samples) * size_t(std::max(1, settings.channels)), 0.f);
        if (!sink.writeSound(silence.data(), samples, error)) return false;
    }
    return sink.finish(error);
}
}  // namespace

int probeMain(int argc, char **argv) {
    answersProbe = true;
    if (argc != 3 || std::strcmp(argv[1], probeFlag) != 0) return -1;
    QCoreApplication app(argc, argv);
    QFile file(QString::fromLocal8Bit(argv[2]));
    QString error = QStringLiteral("The encoder check's request cannot be read.");
    const bool ok = file.open(QIODevice::ReadOnly) && probe(QJsonDocument::fromJson(file.readAll()).toVariant().toMap(), &error);
    if (!ok) std::fprintf(stderr, "%s\n", qPrintable(error));
    return ok ? 0 : 1;
}

QStringList probeCommand(const QVariantMap &spec, int width, int height, int fpsNum, int fpsDen, int colourspace, const QString &output) {
    if (!answersProbe || fpsNum <= 0 || fpsDen <= 0 || QCoreApplication::applicationFilePath().isEmpty()) return {};
    // What the check does not read can be large (a queued job's project, its captions).
    QVariantMap checked = spec;
    for (const char *key : {"snapshot", "snapshotFile", "captionCues", "chapters"}) checked.remove(QLatin1String(key));
    const QString request = output + QStringLiteral(".request.json");
    QFile file(request);
    const QByteArray json = QJsonDocument::fromVariant(QVariantMap{{"spec", checked}, {"width", width}, {"height", height}, {"fpsNum", fpsNum},
                                                                   {"fpsDen", fpsDen}, {"colourspace", colourspace}, {"output", output}})
                                .toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return {};
    return {QCoreApplication::applicationFilePath(), QString::fromLatin1(probeFlag), request};
}

}  // namespace deliver
