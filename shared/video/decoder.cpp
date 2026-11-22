#include "decoder.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QThread>
#include <QMutexLocker>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include <vulkan/vulkan.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}
#include <cstring>
namespace SuiteVideo {
QString hardwareRestriction(const DecodeOptions &options) {
    if (options.backend == "nvdec") {
        // NVDEC runs on the CUDA driver. Without its kernel module there is
        // no device to check, and a decoder-check child would only fail.
        if (!QFileInfo::exists("/dev/nvidiactl") && !QFileInfo::exists("/sys/module/nvidia"))
            return "NVDEC needs the NVIDIA CUDA driver, which is not loaded.";
        return {};
    }
    if (options.backend != "vaapi")
        return {};
    const auto node =
        QFileInfo(options.device.isEmpty() ? QString("/dev/dri/renderD128") : options.device).fileName();
    const auto driver =
        QFileInfo(QFileInfo("/sys/class/drm/" + node + "/device/driver").symLinkTarget()).fileName();
    if (driver == "nvidia" || qEnvironmentVariable("LIBVA_DRIVER_NAME") == "nvidia")
        return "NVIDIA VA-API is disabled after a reproduced seek stall. Use NVDEC for NVIDIA.";
    return {};
}
void enableDriverVideoQueues() {
    QList<QByteArray> flags;
    for (const QByteArray &flag : qgetenv("ANV_DEBUG").split(','))
        if (!flag.trimmed().isEmpty())
            flags.append(flag.trimmed());
    for (const QByteArray &flag : {QByteArray("video-decode"), QByteArray("video-encode")})
        if (!flags.contains(flag))
            flags.append(flag);
    qputenv("ANV_DEBUG", flags.join(','));
}
QStringList vulkanDecodeDevices() {
    QStringList devices;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return devices;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physical(count);
    vkEnumeratePhysicalDevices(instance, &count, physical.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical[i], &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i], &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i], &families, queues.data());
        if (std::any_of(queues.begin(), queues.end(),
                        [](const auto &q) { return q.queueCount > 0 && (q.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR); }))
            devices.append(QString::number(i));
    }
    vkDestroyInstance(instance, nullptr);
    return devices;
}
namespace {
QMutex optionsMutex;
DecodeOptions configuredOptions;
bool configured = false;
std::atomic<quint64> vulkanFrames{0}, vaapiFrames{0}, nvdecFrames{0}, softwareReplays{0}, allSeeks{0}, allDecoded{0};
} // namespace
void setDefaultDecodeOptions(const DecodeOptions &options) {
    QMutexLocker lock(&optionsMutex);
    configuredOptions = options;
    configured = true;
}
DecodeOptions defaultDecodeOptions() {
    QMutexLocker lock(&optionsMutex);
    if (configured)
        return configuredOptions;
    DecodeOptions options;
    const auto selected = qEnvironmentVariable("OMAEDIT_VIDEO_DECODER");
    if (selected == "vaapi")
        options.backend = "vaapi";
    else if (selected == "nvdec" || selected == "cuda")
        options.backend = "nvdec";
    options.device = qEnvironmentVariable("OMAEDIT_VIDEO_DECODER_DEVICE");
    return options;
}
namespace {
// May this stream's pictures hold transparency? Unknown counts as yes. VP8 and
// VP9 carry theirs beside a plain pixel format, marked in the stream's tags.
bool streamHasAlpha(const AVStream *stream) {
    const auto *desc = av_pix_fmt_desc_get(AVPixelFormat(stream->codecpar->format));
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) return true;
    return av_dict_get(stream->metadata, "alpha_mode", nullptr, 0) != nullptr;
}
double streamPixelAspect(AVFormatContext *format, AVStream *stream) {
    const AVRational aspect = av_guess_sample_aspect_ratio(format, stream, nullptr);
    return aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.;
}
}

namespace {
// How a recording's pictures are turned to stand upright: a quarter turn
// swaps the axes, and either axis may run backwards (the display matrix of a
// phone held on its side or upside down, or of a mirrored front camera).
// Upright pixel (x, y) is the recorded pixel (x or w-1-x, y or h-1-y), or
// with the axes swapped the recorded pixel (y or w-1-y, x or h-1-x).
struct Turn {
    bool swap = false, mirrorX = false, mirrorY = false;
    bool none() const { return !swap && !mirrorX && !mirrorY; }
};
// The display matrix as a turn, when it is one of the eight that keep pixels
// on pixels; false for any other (a slant or a scale).
bool turnOf(const int32_t *m, Turn *turn) {
    const auto unit = [](int32_t v) { return std::abs(v - 65536) <= 2 ? 1 : std::abs(v + 65536) <= 2 ? -1 : std::abs(v) <= 2 ? 0 : 2; };
    const int a = unit(m[0]), b = unit(m[1]), c = unit(m[3]), d = unit(m[4]);
    if (a == 2 || b == 2 || c == 2 || d == 2) return false;
    // x' = a·x + c·y and y' = b·x + d·y (libavutil/display.h).
    if (a != 0 && d != 0 && b == 0 && c == 0) { *turn = {false, a < 0, d < 0}; return true; }
    if (b != 0 && c != 0 && a == 0 && d == 0) { *turn = {true, c < 0, b < 0}; return true; }
    return false;
}
const Turn *streamTurn(const AVStream *stream, Turn *turn, bool *supported) {
    *supported = true;
    const auto *matrix = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                                 AV_PKT_DATA_DISPLAYMATRIX);
    if (!matrix || matrix->size < 9 * sizeof(int32_t)) return nullptr;
    *supported = turnOf(reinterpret_cast<const int32_t *>(matrix->data), turn);
    return *supported && !turn->none() ? turn : nullptr;
}
} // namespace

QVariantMap pictureFacts(const QString &path) {
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) return {};
    QVariantMap facts;
    if (avformat_find_stream_info(format, nullptr) >= 0) {
        const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index >= 0) {
            AVStream *stream = format->streams[index];
            facts = {{"width", stream->codecpar->width}, {"height", stream->codecpar->height},
                     {"pixelAspect", streamPixelAspect(format, stream)}, {"alpha", streamHasAlpha(stream)}};
            // As a decoder hands it over: a quarter turn swaps the sides.
            Turn turn;
            bool turnable = true;
            if (const Turn *turned = streamTurn(stream, &turn, &turnable); turned && turned->swap) {
                facts["width"] = stream->codecpar->height;
                facts["height"] = stream->codecpar->width;
                const double aspect = facts.value("pixelAspect").toDouble();
                if (aspect > 0) facts["pixelAspect"] = 1.0 / aspect;
            }
        }
    }
    avformat_close_input(&format);
    return facts;
}

QVariantMap runtimeInformation() {
    return {{"vulkanDownloads", vulkanFrames.load()},
            {"vaapiDownloads", vaapiFrames.load()},
            {"nvdecDownloads", nvdecFrames.load()},
            {"softwareReplays", softwareReplays.load()},
            {"seeks", allSeeks.load()},
            {"decodedPictures", allDecoded.load()}};
}
namespace {
QString message(int code) {
    char value[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, value, sizeof(value));
    return QString::fromUtf8(value);
}
} // namespace
namespace {
// One plane of sw × sh samples of N bytes, turned into dst.
template <int N>
void turnSamples(const uint8_t *src, ptrdiff_t srcStride, int sw, int sh, uint8_t *dst, ptrdiff_t dstStride, const Turn &t) {
    if (!t.swap) {
        for (int dy = 0; dy < sh; ++dy) {
            const uint8_t *from = src + (t.mirrorY ? sh - 1 - dy : dy) * srcStride;
            uint8_t *to = dst + dy * dstStride;
            if (!t.mirrorX) { std::memcpy(to, from, size_t(sw) * N); continue; }
            for (int dx = 0; dx < sw; ++dx) std::memcpy(to + size_t(dx) * N, from + size_t(sw - 1 - dx) * N, N);
        }
        return;
    }
    // By tiles of the source, so the rows read and the columns written both
    // stay in the processor's cache.
    constexpr int tile = 64;
    for (int ty = 0; ty < sh; ty += tile)
        for (int tx = 0; tx < sw; tx += tile)
            for (int sy = ty, endY = std::min(sh, ty + tile); sy < endY; ++sy) {
                const int dx = t.mirrorX ? sh - 1 - sy : sy;
                const uint8_t *from = src + sy * srcStride + size_t(tx) * N;
                for (int sx = tx, endX = std::min(sw, tx + tile); sx < endX; ++sx, from += N)
                    std::memcpy(dst + (t.mirrorY ? sw - 1 - sx : sx) * dstStride + size_t(dx) * N, from, N);
            }
}
bool turnPlane(int bytes, const uint8_t *src, ptrdiff_t srcStride, int sw, int sh, uint8_t *dst, ptrdiff_t dstStride, const Turn &t) {
    switch (bytes) {
    case 1: turnSamples<1>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 2: turnSamples<2>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 3: turnSamples<3>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 4: turnSamples<4>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 6: turnSamples<6>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 8: turnSamples<8>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 12: turnSamples<12>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    case 16: turnSamples<16>(src, srcStride, sw, sh, dst, dstStride, t); return true;
    default: return false;
    }
}
// The bytes of one sample position in each plane (0: no such plane), when
// the format's planes can be turned as they lie: whole bytes, one step a
// plane, and (for a quarter turn) chroma as fine down as across.
bool planeSteps(const AVPixFmtDescriptor *desc, bool swap, int steps[4]) {
    if (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER)) return false;
    if (swap && desc->log2_chroma_w != desc->log2_chroma_h) return false;
    std::fill(steps, steps + 4, 0);
    for (int c = 0; c < desc->nb_components; ++c) {
        const auto &comp = desc->comp[c];
        if (comp.plane < 0 || comp.plane > 3 || comp.step <= 0) return false;
        if (steps[comp.plane] && steps[comp.plane] != comp.step) return false;  // packed 4:2:2 and the like
        steps[comp.plane] = comp.step;
    }
    return true;
}
void clearProps(AVFrame *frame) {
    while (frame->nb_side_data) av_frame_remove_side_data(frame, frame->side_data[0]->type);
    av_dict_free(&frame->metadata);
}
// A frame of this format and size in `frame`, its buffers used again when
// they fit and nobody else holds them.
bool frameFor(AVFrame *frame, int format, int width, int height) {
    if (frame->buf[0] && frame->format == format && frame->width == width && frame->height == height && av_frame_is_writable(frame)) {
        clearProps(frame);
        return true;
    }
    av_frame_unref(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    return av_frame_get_buffer(frame, 0) >= 0;
}
// `from` (in memory) stood upright in `to`. A layout that cannot be turned as
// it lies (4:2:2 by a quarter turn, packed Y'CbCr, a palette) goes through
// `scratch` as 16-bit 4:4:4 first, its range and matrix kept.
bool uprightFrame(const AVFrame *from, AVFrame *to, AVFrame *scratch, SwsContext **scale, const Turn &turn, QString *error) {
    const auto *desc = av_pix_fmt_desc_get(AVPixelFormat(from->format));
    int steps[4];
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER))) {
        *error = "This recording's pictures cannot be turned upright.";
        return false;
    }
    if (!planeSteps(desc, turn.swap, steps)) {
        const bool rgb = desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL);
        const bool alpha = desc->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL);
        const AVPixelFormat wide = rgb ? (alpha ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE) : (alpha ? AV_PIX_FMT_YUVA444P16LE : AV_PIX_FMT_YUV444P16LE);
        *scale = sws_getCachedContext(*scale, from->width, from->height, AVPixelFormat(from->format), from->width, from->height, wide,
                                      SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!*scale || !frameFor(scratch, wide, from->width, from->height)) {
            *error = "This recording's pictures cannot be turned upright.";
            return false;
        }
        const bool full = from->color_range == AVCOL_RANGE_JPEG || (desc->name && std::strncmp(desc->name, "yuvj", 4) == 0);
        if (!rgb) {
            // The same matrix and range on both sides: the codes are only laid out afresh.
            const int *matrix = sws_getCoefficients(from->colorspace == AVCOL_SPC_BT2020_NCL || from->colorspace == AVCOL_SPC_BT2020_CL ? SWS_CS_BT2020
                                                    : from->colorspace == AVCOL_SPC_BT470BG || from->colorspace == AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 : SWS_CS_ITU709);
            sws_setColorspaceDetails(*scale, matrix, full, matrix, full, 0, 1 << 16, 1 << 16);
        }
        if (sws_scale(*scale, from->data, from->linesize, 0, from->height, scratch->data, scratch->linesize) != from->height
            || av_frame_copy_props(scratch, from) < 0) {
            *error = "This recording's pictures cannot be turned upright.";
            return false;
        }
        if (!rgb) scratch->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        from = scratch;
        desc = av_pix_fmt_desc_get(wide);
        if (!planeSteps(desc, turn.swap, steps)) return false;
    }
    const int width = turn.swap ? from->height : from->width, height = turn.swap ? from->width : from->height;
    if (!frameFor(to, from->format, width, height)) {
        *error = "Cannot allocate video decoder frames.";
        return false;
    }
    for (int p = 0; p < 4; ++p) {
        if (!steps[p] || !from->data[p]) continue;
        const bool chroma = p == 1 || p == 2;
        const int sw = chroma ? AV_CEIL_RSHIFT(from->width, desc->log2_chroma_w) : from->width;
        const int sh = chroma ? AV_CEIL_RSHIFT(from->height, desc->log2_chroma_h) : from->height;
        if (!turnPlane(steps[p], from->data[p], from->linesize[p], sw, sh, to->data[p], to->linesize[p], turn)) {
            *error = "This recording's pictures cannot be turned upright.";
            return false;
        }
    }
    if (av_frame_copy_props(to, from) < 0) {
        *error = "Cannot allocate video decoder frames.";
        return false;
    }
    av_frame_remove_side_data(to, AV_FRAME_DATA_DISPLAYMATRIX);  // applied
    if (turn.swap) {
        std::swap(to->sample_aspect_ratio.num, to->sample_aspect_ratio.den);
        // Chroma level with the left luma sample is now level with the top
        // one, and the other way about; fields are columns now, so no fields.
        switch (to->chroma_location) {
        case AVCHROMA_LOC_LEFT: case AVCHROMA_LOC_BOTTOMLEFT: to->chroma_location = AVCHROMA_LOC_TOP; break;
        case AVCHROMA_LOC_TOP: case AVCHROMA_LOC_BOTTOM: to->chroma_location = AVCHROMA_LOC_LEFT; break;
        case AVCHROMA_LOC_UNSPECIFIED:
            if (!(desc->name && std::strncmp(desc->name, "yuvj", 4) == 0) && !(desc->flags & AV_PIX_FMT_FLAG_RGB)) to->chroma_location = AVCHROMA_LOC_TOP;
            break;
        default: break;
        }
        to->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    }
    return true;
}
} // namespace
struct Decoder::State {
    QString path, reason, active = "software";
    DecodeOptions options;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *decoded = av_frame_alloc(), *held = av_frame_alloc(), *download = av_frame_alloc();
    // The held picture stood upright (a recording with a turn), made once.
    AVFrame *upright = av_frame_alloc(), *turnScratch = av_frame_alloc();
    SwsContext *turnScale = nullptr;
    Turn turn;
    bool uprightReady = false;
    AVPacket *packet = av_packet_alloc();
    AVBufferRef *downloadContext = nullptr;
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
    int stream = -1;
    qint64 last = AV_NOPTS_VALUE, deadline = 0;
    quint64 decodedCount = 0, hardwareCount = 0, downloadCount = 0, seekCount = 0, fallbackCount = 0;
    double transferMs = 0;
    bool pending = false, draining = false, ended = false, ready = false, softwareOnly = false;
    bool downloaded = false;
    // A seek that the demuxer answers past the key picture leading to the
    // time asked for (MPEG-TS seeks to any packet) is made again further
    // back; how far back this file needs, kept for its later seeks.
    qint64 preroll = 0, soughtTo = AV_NOPTS_VALUE;
    // A read that failed part-way leaves the decoder somewhere `last` does
    // not say: the next seeks first, unless it is the same request again
    // after a time-out or a cancel (`interrupted`), which carries on.
    bool dirty = false, stalled = false;
    qint64 interrupted = AV_NOPTS_VALUE;
    quint64 damagedCount = 0;
    // Stepping backwards: the last pictures decoded on the way to the one
    // asked for, so the steps that follow are answered from them and a
    // reversed clip decodes each stretch once rather than once a frame. The
    // newest is the picture the decoder has reached; `held` may be an earlier one.
    static constexpr int kKept = 8;
    std::vector<AVFrame *> kept;
    bool reversing = false, heldFromKept = false;
    int forwardRun = 0;
    QVariantMap metadata;
    ~State() {
        close();
        av_packet_free(&packet);
        av_frame_free(&decoded);
        av_frame_free(&held);
        av_frame_free(&download);
        av_frame_free(&upright);
        av_frame_free(&turnScratch);
        sws_freeContext(turnScale);
    }
    void forget() {
        for (AVFrame *&frame : kept) av_frame_free(&frame);
        kept.clear();
        heldFromKept = false;
    }
    void close() {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        av_buffer_unref(&downloadContext);
        if (packet)
            av_packet_unref(packet);
        if (decoded)
            av_frame_unref(decoded);
        if (held)
            av_frame_unref(held);
        if (download)
            av_frame_unref(download);
        if (upright)
            av_frame_unref(upright);
        forget();
        stream = -1;
        last = soughtTo = interrupted = AV_NOPTS_VALUE;
        pending = draining = ended = ready = downloaded = uprightReady = dirty = stalled = false;
        hardwareFormat = AV_PIX_FMT_NONE;
    }
    static int interrupt(void *opaque) {
        auto *s = static_cast<State *>(opaque);
        return s->deadline > 0 && av_gettime_relative() > s->deadline;
    }
    void startDeadline() {
        deadline = av_gettime_relative() + qint64(qBound(100, options.ioTimeoutMs, 30000)) * 1000;
    }
    static AVPixelFormat selectFormat(AVCodecContext *context, const AVPixelFormat *formats) {
        auto *s = static_cast<State *>(context->opaque);
        if (!s->softwareOnly && context->hw_device_ctx)
            for (auto *p = formats; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == s->hardwareFormat)
                    return *p;
        for (auto *p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const auto *desc = av_pix_fmt_desc_get(*p);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *p;
        }
        return AV_PIX_FMT_NONE;
    }
    bool initialize(QString *error) {
        close();
        startDeadline();
        if (!decoded || !held || !download || !upright || !turnScratch || !packet) {
            *error = "Cannot allocate video decoder frames.";
            return false;
        }
        format = avformat_alloc_context();
        if (!format) {
            *error = "Cannot allocate video input.";
            return false;
        }
        format->interrupt_callback = {interrupt, this};
        int code = avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr);
        if (code >= 0)
            code = avformat_find_stream_info(format, nullptr);
        if (code >= 0)
            code = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (code < 0) {
            *error = message(code);
            return false;
        }
        stream = code;
        const auto *parameters = format->streams[stream]->codecpar;
        metadata = {{"codec", QString::fromUtf8(avcodec_get_name(parameters->codec_id))},
                    {"fps", av_q2d(av_guess_frame_rate(format, format->streams[stream], nullptr))},
                    {"width", parameters->width},
                    {"height", parameters->height},
                    {"alpha", streamHasAlpha(format->streams[stream])},
                    {"displayMatrixSupported", true}};
        // Use the same container-duration clock as the native media producer.
        // Unknown duration stays unknown, rather than inventing source handles.
        const double duration = format->duration > 0 && format->duration != AV_NOPTS_VALUE
            ? double(format->duration) / AV_TIME_BASE : 0.;
        const double frames = duration * metadata.value("fps").toDouble();
        if (std::isfinite(frames) && frames > 0 && frames < 2147483647.) {
            metadata["durationSeconds"] = duration;
            metadata["frameCount"] = qMax(1, int(std::llrint(frames)));
        }
        // A recording that is to be shown turned or mirrored: its pictures
        // are handed over upright (output), and its size is the upright one.
        // Only a slanted or scaled display matrix is left unsupported.
        turn = {};
        bool turnable = true;
        streamTurn(format->streams[stream], &turn, &turnable);
        metadata["displayMatrixSupported"] = turnable;
        if (turnable && turn.swap) {
            metadata["width"] = parameters->height;
            metadata["height"] = parameters->width;
        }
        metadata["turned"] = turnable && !turn.none();
        if (parameters->width <= 0 || parameters->height <= 0 ||
            qint64(parameters->width) * parameters->height > 64000000) {
            *error = "Invalid or oversized source video.";
            return false;
        }
        const auto *decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) {
            *error = "No decoder is available for the source video.";
            return false;
        }
        codec = avcodec_alloc_context3(decoder);
        if (!codec) {
            *error = "Cannot allocate source decoder.";
            return false;
        }
        code = avcodec_parameters_to_context(codec, parameters);
        if (code < 0) {
            *error = message(code);
            return false;
        }
        // Software decoding takes half the machine's threads, up to eight:
        // a 1080p ProRes picture in about a third of the time two take, with
        // room left for the other sources a timeline decodes at once.
        codec->thread_count = options.threads > 0 ? std::clamp(options.threads, 1, 16) : std::clamp(QThread::idealThreadCount() / 2, 2, 8);
        codec->opaque = this;
        codec->get_format = selectFormat;
        active = "software";
        // Intra-frame codecs FFmpeg decodes on Vulkan with compute shaders
        // decode on the CPU when their pictures would have to be read back
        // (on an Arc B580 a 1080p 10-bit ProRes picture took 13 ms that way
        // against 3 ms on eight threads, a 4K APV one 84 ms against 8). Left
        // on the GPU for an engine, ProRes decodes there (a 4K picture drawn
        // in 21 ms against 27); APV and FFV1 still do not: their shaders
        // take the GPU from the engine (4K APV 36 ms against 27).
        const auto id = parameters->codec_id;
        const bool shaderDecoded = id == AV_CODEC_ID_PRORES || id == AV_CODEC_ID_FFV1 || id == AV_CODEC_ID_APV;
        if (!softwareOnly && shaderDecoded && (!options.keepVulkanFrames || id != AV_CODEC_ID_PRORES)) {
            softwareOnly = true;
            reason = "This intra-frame codec decodes faster on the CPU; software used.";
        }
        const auto restriction = hardwareRestriction(options);
        if (!softwareOnly && !restriction.isEmpty()) {
            softwareOnly = true;
            reason = restriction;
            ++fallbackCount;
        }
        if (!softwareOnly && options.backend != "software") {
            const auto type = options.backend == "vulkan"  ? AV_HWDEVICE_TYPE_VULKAN
                              : options.backend == "vaapi" ? AV_HWDEVICE_TYPE_VAAPI
                              : options.backend == "nvdec" ? AV_HWDEVICE_TYPE_CUDA
                                                           : AV_HWDEVICE_TYPE_NONE;
            if (type != AV_HWDEVICE_TYPE_NONE)
                for (int i = 0; const auto *config = avcodec_get_hw_config(decoder, i); ++i) {
                    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                        config->device_type == type) {
                        hardwareFormat = config->pix_fmt;
                        break;
                    }
                }
            if (hardwareFormat == AV_PIX_FMT_NONE)
                reason = "The source codec has no compatible hardware configuration; software retained.";
            else if (type == AV_HWDEVICE_TYPE_VULKAN && options.vulkanDevice) {
                codec->hw_device_ctx = av_buffer_ref(options.vulkanDevice);
                if (!codec->hw_device_ctx) {
                    reason = "Hardware decoder initialization failed: " + message(AVERROR(ENOMEM));
                    hardwareFormat = AV_PIX_FMT_NONE;
                    ++fallbackCount;
                }
            } else {
                const auto device = options.device.toUtf8();
                code = av_hwdevice_ctx_create(&codec->hw_device_ctx, type,
                                              device.isEmpty() ? nullptr : device.constData(), nullptr, 0);
                if (code < 0) {
                    reason = "Hardware decoder initialization failed: " + message(code);
                    hardwareFormat = AV_PIX_FMT_NONE;
                    ++fallbackCount;
                }
            }
        }
        code = avcodec_open2(codec, decoder, nullptr);
        if (code < 0) {
            *error = message(code);
            return false;
        }
        ready = true;
        return true;
    }
    const AVFrame *output(QString *error) {
        // VA-API stores its surface in data[3]; data[0] is only meaningful for
        // CPU frames (and some other hardware types).
        if (held->format == AV_PIX_FMT_NONE) {
            *error = "Requested source frame is beyond the decoded video.";
            return nullptr;
        }
        auto *result = held;
        const auto *description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(held->format));
        // (A picture that is to be turned is turned in memory: it comes down
        // from the GPU like any other decoder's.)
        if (options.keepVulkanFrames && held->format == AV_PIX_FMT_VULKAN && turn.none()) {
            active = options.backend;  // left on the GPU for the engine
        } else if (description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            if (!downloaded) {
                // Reuse the CPU transfer surface until the hardware pool or
                // dimensions change. Retained caller images own their pixels.
                if (!downloadContext || downloadContext->data != held->hw_frames_ctx->data ||
                    download->width != held->width || download->height != held->height) {
                    av_frame_unref(download);
                    av_buffer_unref(&downloadContext);
                    downloadContext = av_buffer_ref(held->hw_frames_ctx);
                }
                while (download->nb_side_data)
                    av_frame_remove_side_data(download, download->side_data[0]->type);
                av_dict_free(&download->metadata);
                QElapsedTimer timer;
                timer.start();
                int code = download->buf[0] ? av_frame_make_writable(download) : 0;
                if (code >= 0)
                    code = av_hwframe_transfer_data(download, held, 0);
                transferMs += timer.nsecsElapsed() / 1e6;
                if (code >= 0)
                    code = av_frame_copy_props(download, held);
                if (code < 0) {
                    *error = "Hardware frame download failed: " + message(code);
                    return nullptr;
                }
                ++downloadCount;
                downloaded = true;
                if (options.backend == "vulkan")
                    ++vulkanFrames;
                else if (options.backend == "vaapi")
                    ++vaapiFrames;
                else if (options.backend == "nvdec")
                    ++nvdecFrames;
            }
            result = download;
            active = options.backend;
        } else {
            active = "software";
            if (hardwareFormat != AV_PIX_FMT_NONE && reason.isEmpty())
                reason = "The codec selected software for this stream.";
        }
        const auto *parameters = format->streams[stream]->codecpar;
        if (result->colorspace == AVCOL_SPC_UNSPECIFIED)
            result->colorspace = parameters->color_space;
        if (result->color_range == AVCOL_RANGE_UNSPECIFIED)
            result->color_range = parameters->color_range;
        if (result->chroma_location == AVCHROMA_LOC_UNSPECIFIED)
            result->chroma_location = parameters->chroma_location;
        result->sample_aspect_ratio = av_guess_sample_aspect_ratio(format, format->streams[stream], held);
        if (turn.none()) return result;
        if (!uprightReady) {
            if (!uprightFrame(result, upright, turnScratch, &turnScale, turn, error)) return nullptr;
            uprightReady = true;
        }
        return upright;
    }
    // Starts decoding again at or before `target`, as far back as this file
    // has needed (`preroll`), never before its start.
    bool seekBefore(qint64 target, qint64 origin, QString *error) {
        const qint64 to = target - preroll;
        if (preroll > 0 && to <= origin) {
            // Back to the file's start. A seek to its first time can itself
            // land past the first key picture (which is sent before the time
            // it is shown at): the file is opened afresh instead.
            const bool was = reversing;
            if (!initialize(error))
                return false;
            reversing = was;
            ++seekCount;
            ++allSeeks;
            return true;
        }
        const int code = av_seek_frame(format, stream, to, AVSEEK_FLAG_BACKWARD);
        if (code < 0) {
            *error = message(code);
            return false;
        }
        avcodec_flush_buffers(codec);
        draining = ended = pending = downloaded = uprightReady = false;
        last = AV_NOPTS_VALUE;
        soughtTo = to;
        ++seekCount;
        ++allSeeks;
        av_packet_unref(packet);
        av_frame_unref(held);
        av_frame_unref(decoded);
        av_frame_unref(download);
        forget();
        return true;
    }
    // `frame` becomes the held picture.
    void hold(AVFrame *frame, bool move) {
        av_frame_unref(held);
        if (move) av_frame_move_ref(held, frame);
        else av_frame_ref(held, frame);
        downloaded = uprightReady = false;
    }
    const AVFrame *read(double seconds, QString *error) {
        startDeadline();
        const auto *track = format->streams[stream];
        const qint64 origin = track->start_time == AV_NOPTS_VALUE ? 0 : track->start_time;
        const qint64 target =
            origin + av_rescale_q(qRound64(seconds * 1000000), AVRational{1, 1000000}, track->time_base);
        if (target == last && !dirty)
            return output(error);
        // (Taken now: a seek back to the file's start opens it afresh, and
        // `track` is then the old input's.)
        const double unit = av_q2d(track->time_base);
        const qint64 second = std::max<qint64>(1, av_rescale_q(1000000, AVRational{1, 1000000}, track->time_base));
        // The same request again after a time-out or a cancel: on from where it stopped.
        const bool resume = dirty && interrupted == target;
        const bool back = !dirty && last != AV_NOPTS_VALUE && target < last;
        if (back && (last - target) * unit <= 1.) {
            reversing = true;
            forwardRun = 0;
        }
        if (!dirty && !kept.empty()) {
            const qint64 newest = kept.back()->best_effort_timestamp;
            if (target >= kept.front()->best_effort_timestamp && target < newest) {
                // A step among the pictures kept: the one shown at that time.
                AVFrame *shown = kept.front();
                for (AVFrame *frame : kept)
                    if (frame->best_effort_timestamp <= target) shown = frame;
                hold(shown, false);
                heldFromKept = true;
                last = target;
                return output(error);
            }
            if (heldFromKept && target >= newest) {
                // Forwards past them: the decoder is where the newest was.
                hold(kept.back(), false);
                heldFromKept = false;
                last = newest;
            }
        }
        if (!back && !heldFromKept && ++forwardRun > 2) {
            reversing = false;
            forget();
        }
        // Animated WebP is a canvas assembled from earlier frames. A demuxer
        // seek does not rebuild that canvas; replay from a fresh input on rewind.
        const bool animation = metadata.value("codec").toString() == "webp_anim";
        if (animation && !resume && (dirty || (last != AV_NOPTS_VALUE && target < last))) {
            const bool was = reversing;
            if (!initialize(error)) return nullptr;
            reversing = was;
            ++seekCount;
            ++allSeeks;
        }
        if (!animation && !resume && (dirty || (last != AV_NOPTS_VALUE && target < last) ||
            (target - (last == AV_NOPTS_VALUE ? origin : last)) * unit > 1.)) {
            if (!seekBefore(target, origin, error))
                return nullptr;
        }
        dirty = stalled = false;
        interrupted = AV_NOPTS_VALUE;
        auto result = [&]() {
            const auto *value = output(error);
            if (value)
                last = target;
            else
                dirty = true;
            return value;
        };
        // A read given up part-way: `again` when the same request can carry on.
        auto failed = [&](const QString &why, bool again) -> const AVFrame * {
            *error = why;
            dirty = true;
            stalled = again;
            interrupted = again ? target : AV_NOPTS_VALUE;
            return nullptr;
        };
        int landedLate = 0;
        // After a seek that the demuxer answered too late (the first picture
        // out is already past the time asked for, or none comes before the
        // file ends): again from further back, until it leads to the picture
        // or the file's start is reached.
        const auto furtherBack = [&] {
            if (soughtTo == AV_NOPTS_VALUE || soughtTo <= origin || landedLate >= 8) return false;
            ++landedLate;
            preroll = preroll > 0 ? preroll * 2 : std::max<qint64>(1, second / 2);
            return true;
        };
        for (;;) {
            if (interrupt(this))
                return failed("Video decode timed out.", true);
            if (ended)
                return result();
            int code = pending ? 0 : avcodec_receive_frame(codec, decoded);
            pending = false;
            if (code == 0) {
                // The deadline is for a stall, not for a long way to go.
                startDeadline();
                const auto pts = decoded->best_effort_timestamp;
                const bool first = held->format == AV_PIX_FMT_NONE;
                if (pts != AV_NOPTS_VALUE && pts > target && !first) {
                    pending = true;
                    return result();
                }
                if (pts != AV_NOPTS_VALUE && pts > target && furtherBack()) {
                    av_frame_unref(decoded);
                    if (!seekBefore(target, origin, error))
                        return failed(*error, false);
                    continue;
                }
                soughtTo = AV_NOPTS_VALUE;
                hold(decoded, true);
                ++decodedCount;
                ++allDecoded;
                const auto *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(held->format));
                const bool hardware = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
                if (hardware)
                    ++hardwareCount;
                // (Pictures in memory only: a hardware decoder's come from a
                // pool that keeping them would run dry.)
                if (reversing && !hardware && pts != AV_NOPTS_VALUE) {
                    if (AVFrame *copy = av_frame_clone(held)) kept.push_back(copy);
                    if (int(kept.size()) > kKept) {
                        av_frame_free(&kept.front());
                        kept.erase(kept.begin());
                    }
                } else if (!kept.empty()) {
                    forget();
                }
                if (pts == AV_NOPTS_VALUE || pts >= target)
                    return result();
                if (options.cancelled && options.cancelled())
                    return failed("Cancelled.", true);
                continue;
            }
            if (code == AVERROR_EOF) {
                if (held->format == AV_PIX_FMT_NONE && furtherBack()) {
                    if (!seekBefore(target, origin, error))
                        return failed(*error, false);
                    continue;
                }
                ended = true;
                return result();
            }
            if (code == AVERROR_INVALIDDATA) {
                // A picture the codec could not make of damaged data: left
                // out, and the one before it stands for it.
                ++damagedCount;
                code = AVERROR(EAGAIN);
            }
            if (code != AVERROR(EAGAIN))
                return failed(message(code), false);
            do {
                av_packet_unref(packet);
                code = av_read_frame(format, packet);
            } while (code >= 0 && packet->stream_index != stream);
            if (code == AVERROR_EOF && !draining) {
                draining = true;
                code = avcodec_send_packet(codec, nullptr);
            } else if (code >= 0) {
                startDeadline();
                code = avcodec_send_packet(codec, packet);
                if (code == AVERROR_INVALIDDATA) {
                    ++damagedCount;  // a damaged packet: dropped
                    code = 0;
                }
            }
            if (code < 0) {
                const bool late = interrupt(this);
                return failed(late ? QString("Video decode timed out.") : message(code), late);
            }
        }
    }
};
Decoder::Decoder() : d(std::make_unique<State>()) {}
Decoder::~Decoder() = default;
bool Decoder::open(const QString &path, const DecodeOptions &options, QString *error) {
    QString local;
    if (!error)
        error = &local;
    error->clear();
    d = std::make_unique<State>();
    d->path = path;
    d->options = options;
    if (d->initialize(error))
        return true;
    if (options.backend == "software")
        return false;
    const auto failure = *error;
    d->softwareOnly = true;
    ++d->fallbackCount;
    d->reason = "Hardware open failed; retrying software: " + failure;
    error->clear();
    return d->initialize(error);
}
const AVFrame *Decoder::frame(double seconds, QString *error) {
    QString local;
    if (!error)
        error = &local;
    error->clear();
    if (!d->ready || !std::isfinite(seconds) || seconds < 0 || seconds > 1e9) {
        *error = "Invalid source frame request.";
        return nullptr;
    }
    if (const auto *frame = d->read(seconds, error))
        return frame;
    // A stall or a cancel says nothing against the hardware decoder.
    if (d->stalled || d->softwareOnly || d->hardwareFormat == AV_PIX_FMT_NONE)
        return nullptr;
    const auto failure = *error;
    d->softwareOnly = true;
    ++d->fallbackCount;
    ++softwareReplays;
    d->reason = "Hardware decode failed; replaying through software: " + failure;
    error->clear();
    if (!d->initialize(error))
        return nullptr;
    return d->read(seconds, error);
}
QVariantMap Decoder::information() const {
    auto info = d->metadata;
    info["requested"] = d->options.backend;
    info["device"] = d->options.device;
    info["active"] = d->active;
    info["reason"] = d->reason;
    info["cpuFallback"] = true;
    info["decodedFrames"] = d->decodedCount;
    info["hardwareFrames"] = d->hardwareCount;
    info["downloads"] = d->downloadCount;
    info["seeks"] = d->seekCount;
    info["damaged"] = d->damagedCount;
    info["fallbacks"] = d->fallbackCount;
    info["downloadMs"] = d->transferMs;
    return info;
}
} // namespace SuiteVideo
