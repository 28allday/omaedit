// omaedit --gpu-probe: what the engine finds on this machine, as JSON. Every
// Vulkan device, which one the engine picks and why, whether the engine runs
// on FFmpeg's Vulkan device for it (so decoders and encoders share pictures
// with it), and which Vulkan video codecs actually open there. Availability is
// proven by opening, never by what FFmpeg lists as compiled in.

#include "engine.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace engine {
namespace {

QString kindName(GpuDevice::Kind k) {
    switch (k) {
    case GpuDevice::Kind::Discrete: return QStringLiteral("discrete");
    case GpuDevice::Kind::Integrated: return QStringLiteral("integrated");
    case GpuDevice::Kind::Virtual: return QStringLiteral("virtual");
    case GpuDevice::Kind::Other: break;
    }
    return QStringLiteral("other");
}

QString whyName(GpuPick::Why w) {
    switch (w) {
    case GpuPick::Why::Chosen: return QStringLiteral("chosen");
    case GpuPick::Why::Automatic: return QStringLiteral("automatic");
    case GpuPick::Why::ChosenMissing: return QStringLiteral("chosen device missing");
    case GpuPick::Why::None: break;
    }
    return QStringLiteral("none");
}

QString averror(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return QString::fromUtf8(text);
}

QJsonArray videoCodecs(VkVideoCodecOperationFlagsKHR flags) {
    QJsonArray out;
    const struct { VkVideoCodecOperationFlagsKHR bit; const char *name; } names[] = {
        {VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "decode h264"},
        {VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR, "decode hevc"},
        {VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "decode av1"},
        {VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, "decode vp9"},
        {VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR, "encode h264"},
        {VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR, "encode hevc"},
        {VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR, "encode av1"},
    };
    for (const auto &n : names)
        if (flags & n.bit) out.append(QString::fromLatin1(n.name));
    return out;
}

// One Vulkan encoder opened on the shared device at a small size, fed GPU
// frames of the given layout: "opens" or FFmpeg's reason.
QJsonObject tryEncoder(AVBufferRef *device, const char *name, AVPixelFormat layout) {
    QJsonObject r{{"encoder", QString::fromLatin1(name)}};
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) { r["result"] = QStringLiteral("not in this FFmpeg"); return r; }
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    int error = frames && c ? 0 : AVERROR(ENOMEM);
    if (!error) {
        auto *fc = reinterpret_cast<AVHWFramesContext *>(frames->data);
        fc->format = AV_PIX_FMT_VULKAN;
        fc->sw_format = layout;
        fc->width = 320;
        fc->height = 192;
        error = av_hwframe_ctx_init(frames);
    }
    if (!error) {
        c->width = 320;
        c->height = 192;
        c->time_base = {1, 25};
        c->framerate = {25, 1};
        c->pix_fmt = AV_PIX_FMT_VULKAN;
        c->hw_frames_ctx = av_buffer_ref(frames);
        error = avcodec_open2(c, codec, nullptr);
    }
    r["layout"] = QString::fromLatin1(av_get_pix_fmt_name(layout));
    r["result"] = error ? averror(error) : QStringLiteral("opens");
    avcodec_free_context(&c);
    av_buffer_unref(&frames);
    return r;
}

QJsonObject probeDevice(const GpuDevice &g) {
    QJsonObject d;
    AVBufferRef *device = nullptr;
    int error = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(loaderMutex());
        error = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VULKAN, QByteArray::number(g.index).constData(), nullptr, 0);
    }
    if (error < 0) {
        d["ffmpegDevice"] = averror(error);
        return d;
    }
    const auto *vk = static_cast<const AVVulkanDeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
    QJsonArray families;
    VkVideoCodecOperationFlagsKHR video = 0;
    for (int i = 0; i < vk->nb_qf; ++i) {
        QJsonArray what;
        if (vk->qf[i].flags & VK_QUEUE_GRAPHICS_BIT) what.append(QStringLiteral("graphics"));
        if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT) what.append(QStringLiteral("compute"));
        if (vk->qf[i].flags & VK_QUEUE_TRANSFER_BIT) what.append(QStringLiteral("transfer"));
        if (vk->qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) what.append(QStringLiteral("video decode"));
        if (vk->qf[i].flags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) what.append(QStringLiteral("video encode"));
        video |= vk->qf[i].video_caps;
        families.append(QJsonObject{{"family", vk->qf[i].idx}, {"queues", vk->qf[i].num}, {"does", what}});
    }
    d["ffmpegDevice"] = QStringLiteral("created");
    d["queueFamilies"] = families;
    d["videoCodecs"] = videoCodecs(video);
    std::string why;
    auto shared = createVulkanEngine(g.index, &why, device);
    d["engineOnFfmpegDevice"] = shared ? QStringLiteral("runs") : QString::fromStdString(why);
    shared.reset();
    QJsonArray encoders;
    const struct { const char *name; AVPixelFormat layout; } candidates[] = {
        {"h264_vulkan", AV_PIX_FMT_NV12}, {"hevc_vulkan", AV_PIX_FMT_NV12}, {"hevc_vulkan", AV_PIX_FMT_P010},
        {"av1_vulkan", AV_PIX_FMT_NV12}, {"av1_vulkan", AV_PIX_FMT_P010},
        {"prores_ks_vulkan", AV_PIX_FMT_YUV422P10}, {"ffv1_vulkan", AV_PIX_FMT_YUV444P10},
        {"apv_vulkan", AV_PIX_FMT_YUV422P10},
    };
    for (const auto &c : candidates) encoders.append(tryEncoder(device, c.name, c.layout));
    d["vulkanEncoders"] = encoders;
    {
        std::lock_guard<std::recursive_mutex> lock(loaderMutex());
        av_buffer_unref(&device);
    }
    return d;
}

}  // namespace

int probeMain(int argc, char **argv) {
    bool wanted = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--gpu-probe") == 0) wanted = true;
    if (!wanted) return -1;
    av_log_set_level(AV_LOG_QUIET);
    const auto gpus = listGpus();
    const GpuPick pick = chooseGpu(gpus, {});
    QJsonArray devices;
    for (const GpuDevice &g : gpus) {
        QJsonObject d{{"index", g.index}, {"name", QString::fromStdString(g.name)}, {"id", QString::fromStdString(g.id)},
                      {"kind", kindName(g.kind)}, {"renderNode", QString::fromStdString(g.renderNode)},
                      {"hardware", g.hardware}, {"usable", g.usable}};
        if (g.usable) {
            const QJsonObject more = probeDevice(g);
            for (auto it = more.begin(); it != more.end(); ++it) d[it.key()] = it.value();
        }
        devices.append(d);
    }
    auto engine = Engine::create(Mode::Auto, pick.index);
    const QJsonObject report{
        {"ffmpeg", QString::fromLatin1(av_version_info())},
        {"devices", devices},
        {"pick", QJsonObject{{"index", pick.index}, {"why", whyName(pick.why)}}},
        {"engine", QJsonObject{{"gpu", engine->isGpu()}, {"device", QString::fromStdString(engine->deviceName())},
                               {"note", QString::fromStdString(engine->note())}}},
    };
    const QByteArray text = QJsonDocument(report).toJson(QJsonDocument::Indented);
    std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
    return 0;
}

}  // namespace engine
