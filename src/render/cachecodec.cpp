#include "cachecodec.h"
#include "parallel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace render::cachecodec {
namespace {

QString averror(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return QString::fromUtf8(text);
}

// The device FFmpeg opens for an interface: a render node, an index, or its default.
AVBufferRef *openDevice(AVHWDeviceType type, const QString &device, QString *error) {
    AVBufferRef *made = nullptr;
    const QByteArray name = device.toUtf8();
    const int code = av_hwdevice_ctx_create(&made, type, name.isEmpty() ? nullptr : name.constData(), nullptr, 0);
    if (code < 0) {
        *error = QStringLiteral("The %1 device would not open: %2").arg(QString::fromUtf8(av_hwdevice_get_type_name(type)), averror(code));
        return nullptr;
    }
    return made;
}

AVHWDeviceType typeNamed(const QString &name) {
    return name.isEmpty() || name == QLatin1String("software") ? AV_HWDEVICE_TYPE_NONE
                                                               : av_hwdevice_find_type_by_name(name.toUtf8().constData());
}

// The 4:4:4 10-bit layouts an encoder may take, in the order tried: each
// interface has its own (VA-API packed XV30, NVENC planar 16-bit, ...).
constexpr AVPixelFormat kFullLayouts[] = {AV_PIX_FMT_XV30LE, AV_PIX_FMT_P410LE, AV_PIX_FMT_YUV444P10LE,
                                          AV_PIX_FMT_P416LE, AV_PIX_FMT_YUV444P16LE};

bool offers(const AVPixelFormat *offered, AVPixelFormat f) {
    for (const AVPixelFormat *o = offered; o && *o != AV_PIX_FMT_NONE; ++o)
        if (*o == f) return true;
    return false;
}

// Of `offered` (ending in AV_PIX_FMT_NONE), the first layout this cache
// writes, or `only` when it is named. `encoder` (when given) is the encoder's
// own list: a device's frame pool can hold layouts its encoder cannot read
// (CUDA holds XV30, NVENC takes none), so when that list names any layout
// this cache writes, the layout must be in both.
AVPixelFormat pick(const AVPixelFormat *offered, bool full, const AVPixelFormat *encoder, const QString &only) {
    if (!offered) return AV_PIX_FMT_NONE;
    bool encoderSays = offers(encoder, AV_PIX_FMT_P010LE);
    for (const AVPixelFormat f : kFullLayouts) encoderSays |= offers(encoder, f);
    const auto has = [&](AVPixelFormat f) { return offers(offered, f) && (!encoderSays || offers(encoder, f)); };
    if (!only.isEmpty()) {
        const AVPixelFormat f = av_get_pix_fmt(only.toUtf8().constData());
        return has(f) ? f : AV_PIX_FMT_NONE;
    }
    if (!full) return has(AV_PIX_FMT_P010LE) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NONE;
    for (const AVPixelFormat f : kFullLayouts)
        if (has(f)) return f;
    return AV_PIX_FMT_NONE;
}

uint16_t to10(uint16_t sixteen) { return uint16_t(std::min(1023, (int(sixteen) + 32) >> 6)); }

// The engine's picture (P010, or P416 for 4:4:4) into an FFmpeg frame of the
// encoder's layout, rows on every core.
void fill(AVFrame *to, const engine::YuvFrame &from) {
    const int w = from.width, h = from.height;
    const auto rows = [&](int count, const std::function<void(int)> &row) {
        engine::RowPool::instance().run(0, count, 16, [&](int first, int last) { for (int y = first; y < last; ++y) row(y); });
    };
    const auto src = [&](int plane, int y) {
        return reinterpret_cast<const uint16_t *>((plane ? from.u : from.y).data() + qsizetype(y) * from.rowBytes(plane));
    };
    const auto dst = [&](int plane, int y) { return reinterpret_cast<uint16_t *>(to->data[plane] + qsizetype(y) * to->linesize[plane]); };
    switch (to->format) {
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P416LE:
        for (int plane = 0; plane < 2; ++plane)
            rows(from.rows(plane), [&](int y) { std::memcpy(dst(plane, y), src(plane, y), size_t(from.rowBytes(plane))); });
        return;
    case AV_PIX_FMT_XV30LE:  // (msb) 2X 10V 10Y 10U (lsb)
        rows(h, [&](int y) {
            const uint16_t *l = src(0, y), *c = src(1, y);
            auto *out = reinterpret_cast<uint32_t *>(to->data[0] + qsizetype(y) * to->linesize[0]);
            for (int x = 0; x < w; ++x) out[x] = uint32_t(to10(c[2 * x])) | uint32_t(to10(l[x])) << 10 | uint32_t(to10(c[2 * x + 1])) << 20;
        });
        return;
    case AV_PIX_FMT_P410LE:
        rows(h, [&](int y) {
            const uint16_t *l = src(0, y), *c = src(1, y);
            uint16_t *ol = dst(0, y), *oc = dst(1, y);
            for (int x = 0; x < w; ++x) { ol[x] = uint16_t(to10(l[x]) << 6); oc[2 * x] = uint16_t(to10(c[2 * x]) << 6); oc[2 * x + 1] = uint16_t(to10(c[2 * x + 1]) << 6); }
        });
        return;
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_YUV444P16LE: {
        const bool ten = to->format == AV_PIX_FMT_YUV444P10LE;
        rows(h, [&](int y) {
            const uint16_t *l = src(0, y), *c = src(1, y);
            uint16_t *oy = dst(0, y), *ou = dst(1, y), *ov = dst(2, y);
            for (int x = 0; x < w; ++x) {
                oy[x] = ten ? to10(l[x]) : l[x];
                ou[x] = ten ? to10(c[2 * x]) : c[2 * x];
                ov[x] = ten ? to10(c[2 * x + 1]) : c[2 * x + 1];
            }
        });
        return;
    }
    default: return;
    }
}

// A decoded picture back to the cache's planes (see Planes).
bool take(const AVFrame *from, Planes &to, QString *error) {
    const int w = from->width, h = from->height;
    const auto row = [&](int plane, int y) { return reinterpret_cast<const uint16_t *>(from->data[plane] + qsizetype(y) * from->linesize[plane]); };
    const auto rows = [&](int count, const std::function<void(int)> &each) {
        engine::RowPool::instance().run(0, count, 16, [&](int first, int last) { for (int y = first; y < last; ++y) each(y); });
    };
    to.width = w;
    to.height = h;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (from->format) {
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV420P10LE: {
        const bool packed = from->format == AV_PIX_FMT_P010LE;
        to.full = false;
        to.y.resize(size_t(w) * h);
        to.u.resize(size_t(cw) * 2 * ch);
        to.v.clear();
        rows(h, [&](int y) {
            const uint16_t *in = row(0, y);
            uint16_t *out = to.y.data() + size_t(y) * w;
            for (int x = 0; x < w; ++x) out[x] = packed ? in[x] : uint16_t(in[x] << 6);
        });
        rows(ch, [&](int y) {
            uint16_t *out = to.u.data() + size_t(y) * cw * 2;
            if (packed) { std::memcpy(out, row(1, y), size_t(cw) * 4); return; }
            const uint16_t *cb = row(1, y), *cr = row(2, y);
            for (int x = 0; x < cw; ++x) { out[2 * x] = uint16_t(cb[x] << 6); out[2 * x + 1] = uint16_t(cr[x] << 6); }
        });
        return true;
    }
    case AV_PIX_FMT_XV30LE:
    case AV_PIX_FMT_P410LE:
    case AV_PIX_FMT_P416LE:
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_YUV444P16LE: {
        to.full = true;
        for (auto *plane : {&to.y, &to.u, &to.v}) plane->resize(size_t(w) * h);
        const int format = from->format;
        rows(h, [&](int y) {
            uint16_t *oy = to.y.data() + size_t(y) * w, *ou = to.u.data() + size_t(y) * w, *ov = to.v.data() + size_t(y) * w;
            for (int x = 0; x < w; ++x) {
                if (format == AV_PIX_FMT_XV30LE) {
                    const uint32_t p = reinterpret_cast<const uint32_t *>(from->data[0] + qsizetype(y) * from->linesize[0])[x];
                    ou[x] = uint16_t(p & 1023); oy[x] = uint16_t(p >> 10 & 1023); ov[x] = uint16_t(p >> 20 & 1023);
                } else if (format == AV_PIX_FMT_P410LE || format == AV_PIX_FMT_P416LE) {
                    const int shift = format == AV_PIX_FMT_P410LE ? 6 : 0;
                    const auto ten = [&](uint16_t v) { return shift ? uint16_t(v >> 6) : to10(v); };
                    oy[x] = ten(row(0, y)[x]); ou[x] = ten(row(1, y)[2 * x]); ov[x] = ten(row(1, y)[2 * x + 1]);
                } else {
                    const bool ten = format == AV_PIX_FMT_YUV444P10LE;
                    oy[x] = ten ? row(0, y)[x] : to10(row(0, y)[x]);
                    ou[x] = ten ? row(1, y)[x] : to10(row(1, y)[x]);
                    ov[x] = ten ? row(2, y)[x] : to10(row(2, y)[x]);
                }
            }
        });
        return true;
    }
    default:
        *error = QStringLiteral("The cache decoder gave %1 pictures.").arg(QString::fromUtf8(av_get_pix_fmt_name(AVPixelFormat(from->format))));
        return false;
    }
}

// AV1's hardware decoders hang off FFmpeg's own decoder (not dav1d's).
const AVCodec *decoderFor(int codecId, bool hardware) {
    if (codecId == AV_CODEC_ID_AV1 && hardware)
        if (const AVCodec *own = avcodec_find_decoder_by_name("av1")) return own;
    return avcodec_find_decoder(AVCodecID(codecId));
}

}  // namespace

QVariantMap Choice::toMap() const {
    return {{"encoder", encoder}, {"encodeDevice", encodeDevice}, {"decodeType", decodeType}, {"decodeDevice", decodeDevice},
            {"full", full}, {"picture", picture}, {"msPerFrame", msPerFrame}};
}
Choice Choice::fromMap(const QVariantMap &map) {
    Choice c;
    c.encoder = map.value("encoder").toString();
    c.encodeDevice = map.value("encodeDevice").toString();
    c.decodeType = map.value("decodeType").toString();
    c.decodeDevice = map.value("decodeDevice").toString();
    c.full = map.value("full").toBool();
    c.picture = map.value("picture").toString();
    c.msPerFrame = map.value("msPerFrame").toDouble();
    if (c.encoder.isEmpty() || !avcodec_find_encoder_by_name(c.encoder.toUtf8().constData())) return {};
    return c;
}

// ── Writer ──────────────────────────────────────────────────────────────

struct Writer::State {
    AVCodecContext *codec = nullptr;
    AVBufferRef *device = nullptr, *frames = nullptr;
    AVFrame *picture = nullptr, *uploaded = nullptr;
    AVPacket *packet = nullptr;
    engine::ColourFormat layout = engine::ColourFormat::P010;
    Packet deliver;
    std::map<int64_t, int> frameOf;  // pts → frame
    int64_t next = 0;
    ~State() {
        av_frame_free(&picture);
        av_frame_free(&uploaded);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
    }
    bool drain(QString *error) {
        for (;;) {
            const int got = avcodec_receive_packet(codec, packet);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
            if (got < 0) { *error = QStringLiteral("The cache encoder failed: ") + averror(got); return false; }
            const auto it = frameOf.find(packet->pts);
            const int frame = it == frameOf.end() ? -1 : it->second;
            if (it != frameOf.end()) frameOf.erase(it);
            const QByteArray bytes(reinterpret_cast<const char *>(packet->data), packet->size);
            av_packet_unref(packet);
            if (frame >= 0 && deliver && !deliver(frame, bytes)) { *error = QStringLiteral("The cache could not keep a picture."); return false; }
        }
    }
    bool send(AVFrame *frame, QString *error) {
        for (;;) {
            const int sent = avcodec_send_frame(codec, frame);
            if (sent == AVERROR(EAGAIN)) { if (!drain(error)) return false; continue; }
            if (sent < 0 && sent != AVERROR_EOF) { *error = QStringLiteral("The cache encoder refused a picture: ") + averror(sent); return false; }
            return drain(error);
        }
    }
};

Writer::Writer() : d(std::make_unique<State>()) {}
Writer::~Writer() = default;
int Writer::codecId() const { return d->codec ? int(d->codec->codec_id) : 0; }
QByteArray Writer::extradata() const {
    return d->codec && d->codec->extradata ? QByteArray(reinterpret_cast<const char *>(d->codec->extradata), d->codec->extradata_size) : QByteArray();
}

bool Writer::open(const Choice &choice, int width, int height, Packet packet, QString *error) {
    auto &s = *d;
    const AVCodec *encoder = avcodec_find_encoder_by_name(choice.encoder.toUtf8().constData());
    if (!encoder) { *error = QStringLiteral("No %1 encoder.").arg(choice.encoder); return false; }
    s.codec = avcodec_alloc_context3(encoder);
    s.packet = av_packet_alloc();
    s.picture = av_frame_alloc();
    if (!s.codec || !s.packet || !s.picture) { *error = QStringLiteral("Out of memory for the cache encoder."); return false; }
    s.deliver = std::move(packet);
    s.layout = choice.layout();
    AVCodecContext *c = s.codec;
    c->width = width;
    c->height = height;
    c->time_base = {1, 25};
    c->framerate = {25, 1};
    c->gop_size = 1;  // every picture its own
    c->max_b_frames = 0;
    c->color_range = AVCOL_RANGE_JPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_UNSPECIFIED;  // the cache's own log curve
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_LOW_DELAY;
    AVPixelFormat layout = AV_PIX_FMT_NONE;
    const AVPixelFormat *encoderFormats = nullptr;
    avcodec_get_supported_config(c, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0, reinterpret_cast<const void **>(&encoderFormats), nullptr);
    // A device the encoder takes pictures on, when it has one; else memory.
    for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(encoder, i); ++i) {
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)) continue;
        QString why;
        AVBufferRef *device = openDevice(config->device_type, choice.encodeDevice, &why);
        if (!device) continue;
        AVHWFramesConstraints *limits = av_hwdevice_get_hwframe_constraints(device, nullptr);
        layout = limits ? pick(limits->valid_sw_formats, choice.full, encoderFormats, choice.picture) : AV_PIX_FMT_NONE;
        av_hwframe_constraints_free(&limits);
        AVBufferRef *frames = layout == AV_PIX_FMT_NONE ? nullptr : av_hwframe_ctx_alloc(device);
        auto *fc = frames ? reinterpret_cast<AVHWFramesContext *>(frames->data) : nullptr;
        if (fc) {
            fc->format = config->pix_fmt;
            fc->sw_format = layout;
            fc->width = width;
            fc->height = height;
            fc->initial_pool_size = 4;
        }
        if (!fc || av_hwframe_ctx_init(frames) < 0) { av_buffer_unref(&frames); av_buffer_unref(&device); layout = AV_PIX_FMT_NONE; continue; }
        s.device = device;
        s.frames = frames;
        c->pix_fmt = config->pix_fmt;
        c->hw_frames_ctx = av_buffer_ref(frames);
        break;
    }
    if (!s.frames) {
        layout = pick(encoderFormats, choice.full, nullptr, choice.picture);
        c->pix_fmt = layout;
    }
    if (layout == AV_PIX_FMT_NONE) {
        *error = QStringLiteral("The %1 encoder takes no %2 10-bit pictures here.").arg(choice.encoder, choice.full ? "4:4:4" : "4:2:0");
        return false;
    }
    c->sw_pix_fmt = layout;
    // Near-lossless and immediate, in each interface's own words (unknown
    // names are left unused).
    const bool av1 = encoder->id == AV_CODEC_ID_AV1;
    const QByteArray qp = av1 ? "24" : "12";
    AVDictionary *options = nullptr;
    const QString name = choice.encoder;
    if (name.endsWith(QLatin1String("_nvenc"))) {
        // NVENC's own word for every picture its own is no group at all: a
        // group of one is refused ("greater than B frames + 1").
        c->gop_size = 0;
        av_dict_set(&options, "rc", "constqp", 0);
        av_dict_set(&options, "qp", qp.constData(), 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "zerolatency", "1", 0);
    } else if (name.endsWith(QLatin1String("_amf"))) {
        av_dict_set(&options, "rc", "cqp", 0);
        av_dict_set(&options, "qp_i", qp.constData(), 0);
        av_dict_set(&options, "qp_p", qp.constData(), 0);
    } else {
        av_dict_set(&options, "rc_mode", "CQP", 0);
        av_dict_set(&options, "qp", qp.constData(), 0);
        av_dict_set(&options, "async_depth", "1", 0);
    }
    const int opened = avcodec_open2(c, encoder, &options);
    av_dict_free(&options);
    if (opened < 0) { *error = QStringLiteral("The %1 encoder would not open: %2").arg(name, averror(opened)); return false; }
    s.picture->format = layout;
    s.picture->width = width;
    s.picture->height = height;
    if (av_frame_get_buffer(s.picture, 0) < 0) { *error = QStringLiteral("Out of memory for cache pictures."); return false; }
    if (s.frames && !(s.uploaded = av_frame_alloc())) { *error = QStringLiteral("Out of memory for cache pictures."); return false; }
    return true;
}

bool Writer::encode(const engine::YuvFrame &picture, int frame, QString *error) {
    auto &s = *d;
    if (!s.codec || picture.format != s.layout || picture.width != s.codec->width || picture.height != s.codec->height) {
        *error = QStringLiteral("A picture of the wrong shape reached the cache encoder.");
        return false;
    }
    if (av_frame_make_writable(s.picture) < 0) { *error = QStringLiteral("Out of memory for cache pictures."); return false; }
    fill(s.picture, picture);
    s.picture->pts = s.next;
    s.frameOf[s.next++] = frame;
    s.picture->color_range = AVCOL_RANGE_JPEG;
    s.picture->colorspace = AVCOL_SPC_BT709;
    if (!s.frames) return s.send(s.picture, error);
    av_frame_unref(s.uploaded);
    int up = av_hwframe_get_buffer(s.frames, s.uploaded, 0);
    if (up >= 0) up = av_hwframe_transfer_data(s.uploaded, s.picture, 0);
    if (up >= 0) up = av_frame_copy_props(s.uploaded, s.picture);
    if (up < 0) { *error = QStringLiteral("A picture could not reach the cache encoder: ") + averror(up); return false; }
    return s.send(s.uploaded, error);
}

bool Writer::finish(QString *error) { return !d->codec || d->send(nullptr, error); }

// ── Reader ──────────────────────────────────────────────────────────────

struct Reader::State {
    AVCodecContext *codec = nullptr;
    AVFrame *decoded = nullptr, *downloaded = nullptr;
    AVPacket *packet = nullptr;
    AVPixelFormat hardware = AV_PIX_FMT_NONE;
    ~State() {
        av_frame_free(&decoded);
        av_frame_free(&downloaded);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
    }
};

Reader::Reader() : d(std::make_unique<State>()) {}
Reader::~Reader() = default;

bool Reader::open(const Choice &choice, int codecId, const QByteArray &extradata, QString *error) {
    auto &s = *d;
    const AVHWDeviceType type = typeNamed(choice.decodeType);
    const AVCodec *decoder = decoderFor(codecId, type != AV_HWDEVICE_TYPE_NONE);
    if (!decoder) { *error = QStringLiteral("No decoder for the cache's pictures."); return false; }
    s.codec = avcodec_alloc_context3(decoder);
    s.decoded = av_frame_alloc();
    s.downloaded = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.codec || !s.decoded || !s.downloaded || !s.packet) { *error = QStringLiteral("Out of memory for the cache decoder."); return false; }
    AVCodecContext *c = s.codec;
    if (!extradata.isEmpty()) {
        c->extradata = static_cast<uint8_t *>(av_mallocz(size_t(extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!c->extradata) { *error = QStringLiteral("Out of memory for the cache decoder."); return false; }
        std::memcpy(c->extradata, extradata.constData(), size_t(extradata.size()));
        c->extradata_size = int(extradata.size());
    }
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (type != AV_HWDEVICE_TYPE_NONE) {
        for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i); ++i)
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type) {
                s.hardware = config->pix_fmt;
                break;
            }
        if (s.hardware == AV_PIX_FMT_NONE) { *error = QStringLiteral("This decoder has no %1 interface.").arg(choice.decodeType); return false; }
        c->hw_device_ctx = openDevice(type, choice.decodeDevice, error);
        if (!c->hw_device_ctx) return false;
        c->opaque = &s;
        c->get_format = [](AVCodecContext *context, const AVPixelFormat *offered) {
            const auto *state = static_cast<const State *>(context->opaque);
            for (const AVPixelFormat *f = offered; *f != AV_PIX_FMT_NONE; ++f)
                if (*f == state->hardware) return *f;
            return AV_PIX_FMT_NONE;  // no quiet fall back to software: it was not what was measured
        };
    }
    const int opened = avcodec_open2(c, decoder, nullptr);
    if (opened < 0) { *error = QStringLiteral("The cache decoder would not open: ") + averror(opened); return false; }
    return true;
}

bool Reader::decode(const QByteArray &packet, Planes &picture, QString *error) {
    auto &s = *d;
    if (!s.codec) { *error = QStringLiteral("The cache decoder is not open."); return false; }
    av_packet_unref(s.packet);
    if (av_new_packet(s.packet, int(packet.size())) < 0) { *error = QStringLiteral("Out of memory for a cache picture."); return false; }
    std::memcpy(s.packet->data, packet.constData(), size_t(packet.size()));
    s.packet->flags |= AV_PKT_FLAG_KEY;
    int code = avcodec_send_packet(s.codec, s.packet);
    if (code < 0) { *error = QStringLiteral("The cache decoder refused a picture: ") + averror(code); return false; }
    code = avcodec_receive_frame(s.codec, s.decoded);
    if (code == AVERROR(EAGAIN)) {
        // A decoder that holds pictures back: drained, then made ready again.
        avcodec_send_packet(s.codec, nullptr);
        code = avcodec_receive_frame(s.codec, s.decoded);
        avcodec_flush_buffers(s.codec);
    }
    if (code < 0) { *error = QStringLiteral("The cache decoder failed: ") + averror(code); return false; }
    const AVFrame *result = s.decoded;
    if (s.decoded->hw_frames_ctx) {
        av_frame_unref(s.downloaded);
        code = av_hwframe_transfer_data(s.downloaded, s.decoded, 0);
        if (code < 0) { av_frame_unref(s.decoded); *error = QStringLiteral("A cache picture could not be read back: ") + averror(code); return false; }
        result = s.downloaded;
    }
    const bool ok = take(result, picture, error);
    av_frame_unref(s.decoded);
    return ok;
}

// ── Qualification ───────────────────────────────────────────────────────

namespace {

constexpr int kProbeWidth = 1920, kProbeHeight = 1080, kProbeFrames = 10;

// A test picture with what a cache must keep: smooth ramps, fine texture
// and saturated colour, in the layout the engine makes. Picture n moves its
// texture a little.
engine::YuvFrame testPicture(int n, bool full) {
    engine::YuvFrame p;
    p.resize(kProbeWidth, kProbeHeight, full ? engine::ColourFormat::P416 : engine::ColourFormat::P010);
    uint32_t seed = 12345u + uint32_t(n) * 7919u;
    const auto noise = [&] { seed = seed * 1664525u + 1013904223u; return int(seed >> 28) - 8; };
    for (int y = 0; y < p.height; ++y) {
        auto *row = reinterpret_cast<uint16_t *>(p.y.data() + qsizetype(y) * p.rowBytes(0));
        for (int x = 0; x < p.width; ++x) {
            const int ramp = 64 + (x * 896) / p.width + ((y / 60) % 2) * 32;
            row[x] = uint16_t(std::clamp(ramp + noise(), 0, 1023) << 6);
        }
    }
    for (int y = 0; y < p.chromaHeight(); ++y) {
        auto *row = reinterpret_cast<uint16_t *>(p.u.data() + qsizetype(y) * p.rowBytes(1));
        for (int x = 0; x < p.chromaWidth(); ++x) {
            const int bar = (x * 8) / p.chromaWidth();
            row[2 * x] = uint16_t(std::clamp(512 + (bar - 4) * 90 + noise(), 0, 1023) << 6);
            row[2 * x + 1] = uint16_t(std::clamp(512 + ((bar * 3) % 8 - 4) * 90 + noise(), 0, 1023) << 6);
        }
    }
    return p;
}

// The test picture as a reader gives pictures back, to compare with them.
Planes asRead(const engine::YuvFrame &p) {
    Planes out;
    out.width = p.width;
    out.height = p.height;
    out.full = p.format == engine::ColourFormat::P416;
    const auto word = [&](int plane, int y, int i) {
        return reinterpret_cast<const uint16_t *>((plane ? p.u : p.y).data() + qsizetype(y) * p.rowBytes(plane))[i];
    };
    out.y.resize(size_t(p.width) * p.height);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) out.y[size_t(y) * p.width + x] = out.full ? uint16_t(word(0, y, x) >> 6) : word(0, y, x);
    if (out.full) {
        out.u.resize(out.y.size());
        out.v.resize(out.y.size());
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x) {
                out.u[size_t(y) * p.width + x] = uint16_t(word(1, y, 2 * x) >> 6);
                out.v[size_t(y) * p.width + x] = uint16_t(word(1, y, 2 * x + 1) >> 6);
            }
    } else {
        out.u.resize(size_t(p.chromaWidth()) * 2 * p.chromaHeight());
        for (int y = 0; y < p.chromaHeight(); ++y)
            for (int i = 0; i < p.chromaWidth() * 2; ++i) out.u[size_t(y) * p.chromaWidth() * 2 + size_t(i)] = word(1, y, i);
    }
    return out;
}

// Peak signal-to-noise in dB on 10-bit codes: luma and chroma.
std::pair<double, double> psnr(const Planes &a, const Planes &b) {
    if (a.full != b.full || a.y.size() != b.y.size() || a.u.size() != b.u.size() || a.v.size() != b.v.size()) return {0, 0};
    const int shift = a.full ? 0 : 6;  // P010 keeps its codes in the high bits
    const auto err = [&](const std::vector<uint16_t> &x, const std::vector<uint16_t> &y) {
        double e = 0;
        for (size_t i = 0; i < x.size(); ++i) { const double d = double(x[i] >> shift) - double(y[i] >> shift); e += d * d; }
        return e;
    };
    const auto db = [](double e, size_t n) { return n && e > 0 ? 10 * std::log10(1023.0 * 1023.0 / (e / double(n))) : 99.0; };
    return {db(err(a.y, b.y), a.y.size()), db(err(a.u, b.u) + err(a.v, b.v), a.u.size() + a.v.size())};
}

// The decoders worth trying for an encoder's pictures: its own interface
// first, then any other the machine has, and software.
QList<QPair<QString, QString>> decodersFor(const QString &encoder, const QString &device) {
    QList<QPair<QString, QString>> list;
    if (encoder.endsWith(QLatin1String("_vaapi"))) list.append({"vaapi", device});
    if (encoder.endsWith(QLatin1String("_nvenc"))) list.append({"cuda", device});
    if (encoder.endsWith(QLatin1String("_vulkan"))) list.append({"vulkan", device});
    list.append({"vulkan", QString()});
    for (const auto &node : QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name).mid(0, 3))
        list.append({"vaapi", "/dev/dri/" + node});
    if (QFileInfo::exists("/dev/nvidiactl")) list.append({"cuda", QString()});
    list.append({"software", QString()});
    QList<QPair<QString, QString>> unique;
    for (const auto &d : list) if (!unique.contains(d)) unique.append(d);
    return unique;
}

QVariantMap probe(const QString &encoder, const QString &device, bool full) {
    QVariantMap report{{"encoder", encoder}, {"device", device}, {"full", full}, {"valid", false}};
    Choice choice;
    choice.encoder = encoder;
    choice.encodeDevice = device;
    choice.full = full;
    std::vector<engine::YuvFrame> pictures;
    pictures.reserve(kProbeFrames);
    for (int n = 0; n < kProbeFrames; ++n) pictures.push_back(testPicture(n, full));
    // Each full-colour layout in turn, until one the encoder really takes: an
    // encoder can list a layout and then refuse its pictures.
    QStringList layouts{QString()};
    if (full) {
        layouts.clear();
        for (const AVPixelFormat f : kFullLayouts) layouts << QString::fromUtf8(av_get_pix_fmt_name(f));
    }
    std::map<int, QByteArray> packets;
    std::unique_ptr<Writer> writer;
    QString error;
    QElapsedTimer timer;
    QStringList refused;
    for (const QString &layout : layouts) {
        packets.clear();
        choice.picture = layout;
        writer = std::make_unique<Writer>();
        QString why;
        bool ok = writer->open(choice, kProbeWidth, kProbeHeight, [&](int frame, const QByteArray &p) { packets[frame] = p; return true; }, &why);
        timer.start();
        for (int n = 0; ok && n < kProbeFrames; ++n) ok = writer->encode(pictures[size_t(n)], n, &why);
        if (ok && (!writer->finish(&why) || int(packets.size()) != kProbeFrames)) {
            ok = false;
            if (why.isEmpty()) why = QStringLiteral("The encoder lost pictures.");
        }
        if (ok) { error.clear(); break; }
        error = why;
        if (!layout.isEmpty()) refused << layout + ": " + why;
        writer.reset();
    }
    if (!writer) {
        report["error"] = error;
        if (!refused.isEmpty()) report["refused"] = refused;
        return report;
    }
    report["encodeMs"] = double(timer.nsecsElapsed()) / 1e6 / kProbeFrames;
    report["picture"] = choice.picture;
    if (!refused.isEmpty()) report["refused"] = refused;
    qint64 bytes = 0;
    for (const auto &[n, p] : packets) bytes += p.size();
    report["bytesPerFrame"] = bytes / kProbeFrames;
    std::vector<Planes> expected;
    expected.reserve(pictures.size());
    for (const auto &p : pictures) expected.push_back(asRead(p));
    QVariantList decoders;
    for (const auto &[type, node] : decodersFor(encoder, device)) {
        QVariantMap tried{{"type", type}, {"device", node}, {"valid", false}};
        Choice reading = choice;
        reading.decodeType = type;
        reading.decodeDevice = node;
        Reader reader;
        QString why;
        if (!reader.open(reading, writer->codecId(), writer->extradata(), &why)) { tried["error"] = why; decoders.append(tried); continue; }
        Planes back;
        double worstY = 99, worstC = 99;
        bool ok = true;
        timer.restart();
        // In an order playback might ask for: each picture alone.
        for (int n : {0, 5, 1, 9, 2, 7, 3, 8, 4, 6}) {
            if (!reader.decode(packets[n], back, &why)) { ok = false; break; }
            const auto [y, c] = psnr(expected[size_t(n)], back);
            worstY = std::min(worstY, y);
            worstC = std::min(worstC, c);
        }
        if (!ok) { tried["error"] = why; decoders.append(tried); continue; }
        tried["decodeMs"] = double(timer.nsecsElapsed()) / 1e6 / kProbeFrames;
        tried["psnrY"] = worstY;
        tried["psnrC"] = worstC;
        // Faithful: what a viewer cannot tell from the frame it came from.
        tried["valid"] = worstY >= 45 && worstC >= 40;
        decoders.append(tried);
    }
    report["decoders"] = decoders;
    report["valid"] = true;
    return report;
}

// What the answer depends on: the encoders FFmpeg has and the devices there are.
QString machineKey() {
    QStringList parts{QCoreApplication::applicationVersion(), QString::fromUtf8(av_version_info()), QStringLiteral("444 layouts"),  // the way of probing: a change re-probes
                      qEnvironmentVariable("OMAEDIT_CACHE_CHROMA")};
    for (const QString codec : {"hevc", "av1"})
        for (const QString api : {"vulkan", "vaapi", "nvenc", "amf"})
            if (avcodec_find_encoder_by_name((codec + '_' + api).toUtf8().constData())) parts << codec + '_' + api;
    for (const auto &node : QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name)) {
        QFile vendor("/sys/class/drm/" + node + "/device/device");
        parts << node + (vendor.open(QIODevice::ReadOnly) ? QString::fromUtf8(vendor.readAll().trimmed()) : QString());
    }
    parts << (QFileInfo::exists("/dev/nvidiactl") ? "cuda" : "");
    return QString::fromLatin1(QCryptographicHash::hash(parts.join('|').toUtf8(), QCryptographicHash::Sha256).toHex());
}

}  // namespace

int probeMain(int argc, char **argv) {
    if (argc < 3 || QByteArray(argv[1]) != "--omaedit-cache-probe") return -1;
    QCoreApplication app(argc, argv);
    const bool full = argc > 4 && QByteArray(argv[4]) == "444";
    const QVariantMap report = probe(QString::fromUtf8(argv[2]), argc > 3 ? QString::fromUtf8(argv[3]) : QString(), full);
    QFile out;
    if (!out.open(stdout, QIODevice::WriteOnly)) return 2;
    out.write(QJsonDocument::fromVariant(report).toJson(QJsonDocument::Compact));
    out.write("\n");
    return report.value("valid").toBool() ? 0 : 1;
}

Choice choose(QVariantMap *report) {
    const QString key = machineKey();
    QSettings settings;
    const QVariantMap kept = settings.value(QStringLiteral("playback/cacheCodec")).toMap();
    const qint64 age = QDateTime::currentSecsSinceEpoch() - kept.value("checkedAt").toLongLong();
    if (kept.value("key").toString() == key && age >= 0 && age < 86400) {
        if (report) *report = kept.value("report").toMap();
        return Choice::fromMap(kept.value("choice").toMap());
    }
    // Every hardware encoder FFmpeg has here, on every device it might use,
    // in full colour and in 4:2:0.
    struct Candidate { QString encoder, device; bool full; };
    QList<Candidate> candidates;
    const QStringList nodes = QDir("/dev/dri").entryList({"renderD*"}, QDir::System | QDir::Files, QDir::Name).mid(0, 3);
    for (const bool full : {true, false})
        for (const QString codec : {"hevc", "av1"}) {
            const auto have = [&](const QString &api) { return avcodec_find_encoder_by_name((codec + '_' + api).toUtf8().constData()) != nullptr; };
            if (have("vulkan")) candidates.append({codec + "_vulkan", QString(), full});
            if (have("vaapi"))
                for (const auto &node : nodes) candidates.append({codec + "_vaapi", "/dev/dri/" + node, full});
            // NVENC needs NVIDIA's driver loaded; without it a check only fails.
            if (have("nvenc") && QFileInfo::exists("/dev/nvidiactl")) candidates.append({codec + "_nvenc", QString(), full});
            if (have("amf")) candidates.append({codec + "_amf", QString(), full});
        }
    QVariantList tried;
    // The best pair in full colour and in 4:2:0: hardware decoding whenever
    // it works (software reads the pictures back only when no GPU interface
    // can), then the quickest.
    Choice best[2];  // [0] 4:2:0, [1] 4:4:4
    const auto better = [](const Choice &mine, const Choice &theirs) {
        if (!theirs.valid()) return true;
        const bool a = mine.decodeType == QLatin1String("software"), b = theirs.decodeType == QLatin1String("software");
        return a != b ? !a : mine.msPerFrame < theirs.msPerFrame;
    };
    QElapsedTimer deadline;
    deadline.start();
    for (const auto &[encoder, device, full] : candidates) {
        const int remaining = 90000 - int(deadline.elapsed());
        if (remaining <= 0) break;
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--omaedit-cache-probe", encoder, device, full ? "444" : "420"});
        QVariantMap result{{"encoder", encoder}, {"device", device}, {"full", full}, {"valid", false}};
        if (!child.waitForStarted(3000) || !child.waitForFinished(std::min(10000, remaining))) {
            child.kill();
            child.waitForFinished(1000);
            result["error"] = QStringLiteral("The check did not finish.");
        } else {
            const auto lines = child.readAllStandardOutput().trimmed().split('\n');
            const auto parsed = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.last()).toVariant().toMap();
            if (!parsed.isEmpty()) result = parsed;
        }
        if (result.value("valid").toBool())
            for (const QVariant &v : result.value("decoders").toList()) {
                const QVariantMap dec = v.toMap();
                if (!dec.value("valid").toBool()) continue;
                Choice pair;
                pair.encoder = encoder;
                pair.encodeDevice = device;
                pair.decodeType = dec.value("type").toString();
                pair.decodeDevice = dec.value("device").toString();
                pair.full = full;
                pair.picture = result.value("picture").toString();
                pair.msPerFrame = result.value("encodeMs").toDouble() + dec.value("decodeMs").toDouble();
                if (better(pair, best[full])) best[full] = pair;
            }
        tried.append(result);
    }
    // Full colour unless it costs half as much again as 4:2:0 would: the
    // cache is there for smooth playback.
    // OMAEDIT_CACHE_CHROMA=444 or 420 takes that one alone (none: lossless).
    const QString forced = qEnvironmentVariable("OMAEDIT_CACHE_CHROMA");
    const Choice chosen = forced == QLatin1String("444") ? best[1] : forced == QLatin1String("420") ? best[0]
                        : best[1].valid() && (!best[0].valid() || best[1].msPerFrame <= best[0].msPerFrame * 1.5) ? best[1] : best[0];
    const QVariantMap made{{"candidates", tried},
                           {"chosen", chosen.toMap()},
                           {"reason", chosen.valid() ? chosen.full ? QStringLiteral("Full colour (4:4:4): the quickest hardware encoder and decoder that give it back faithfully.")
                                                                   : QStringLiteral("4:2:0: full colour was unavailable or much slower here; the quickest hardware encoder and decoder.")
                                                   : QStringLiteral("No hardware encoder gave the picture back faithfully: lossless frames kept.")}};
    settings.setValue(QStringLiteral("playback/cacheCodec"),
                      QVariantMap{{"key", key}, {"checkedAt", QDateTime::currentSecsSinceEpoch()}, {"choice", chosen.toMap()}, {"report", made}});
    if (report) *report = made;
    return chosen;
}

}  // namespace render::cachecodec
