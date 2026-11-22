#include "stabilise.h"
#include "framing.h"

#include <QCoreApplication>
#include <QFile>
#include <algorithm>
#include <atomic>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <cmath>
#include <cstdio>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <vid.stab/libvidstab.h>
}

#include <QImage>

namespace render {

struct MotionAnalysis::State {
    VSMotionDetect detect{};
    VSFrameInfo info{};
    bool detecting = false;
    FILE *file = nullptr;
    SwsContext *scale = nullptr;
    std::vector<uint8_t> planes;
    QSize size;
    ~State() {
        if (detecting) vsMotionDetectionCleanup(&detect);
        if (file) std::fclose(file);
        sws_freeContext(scale);
    }
};

MotionAnalysis::MotionAnalysis() : d(std::make_unique<State>()) {}
MotionAnalysis::~MotionAnalysis() = default;

bool MotionAnalysis::open(const QString &results, int shakiness, const QSize &size, QString *error) {
    auto &s = *d;
    s.size = size;
    if (size.width() < 2 || size.height() < 2 || size.width() % 2 || size.height() % 2) {
        // (The analysis library stops the program on an odd size.)
        if (error) *error = QCoreApplication::translate("Stabilise", "The motion analysis cannot start.");
        return false;
    }
    VSMotionDetectConfig conf = vsMotionDetectGetDefaultConfig("vidstab");
    conf.shakiness = shakiness;
    conf.accuracy = 4;
    conf.stepSize = 6;
    conf.contrastThreshold = 0.3;
    conf.show = 0;
    conf.virtualTripod = 0;
    // Planar 4:4:4: the analysis sees full chroma, as it always has.
    vsFrameInfoInit(&s.info, size.width(), size.height(), PF_YUV444P);
    if (vsMotionDetectInit(&s.detect, &conf, &s.info) != VS_OK) {
        if (error) *error = QCoreApplication::translate("Stabilise", "The motion analysis cannot start.");
        return false;
    }
    s.detecting = true;
    s.file = std::fopen(QFile::encodeName(results).constData(), "w");
    if (!s.file || vsPrepareFile(&s.detect, s.file) != VS_OK) {
        if (error) *error = QCoreApplication::translate("Stabilise", "The stabilisation analysis file cannot be written.");
        return false;
    }
    s.planes.assign(size_t(size.width()) * size.height() * 3, 0);
    return true;
}

QSize analysisFrame(const QString &clipScaling, const QString &timelineScaling, int w, int h, double ratio, const QSize &timeline) {
    const Scale scale = scaleInto(scalingFor(clipScaling, timelineScaling), w, h, ratio, timeline.width(), timeline.height());
    // Even (the analysis takes no odd size), no smaller than it can find
    // anything in, no larger than a frame can be.
    const auto side = [](double v) { return std::clamp(2 * int(std::lround(v / 2)), 32, 16384); };
    return QSize(side(w * scale.x), side(h * scale.y));
}

QString analysisFileName(const QString &stem, const QSize &picture) {
    return QStringLiteral("%1-%2x%3.trf").arg(stem).arg(picture.width()).arg(picture.height());
}

namespace {
// The picture size a results file's name gives (analysisFileName), if it does.
QSize pictureOf(const QString &results) {
    static const QRegularExpression named(QStringLiteral("-(\\d{1,5})x(\\d{1,5})\\.trf$"));
    const auto match = named.match(QFileInfo(results).fileName());
    if (!match.hasMatch()) return {};
    const QSize size(match.captured(1).toInt(), match.captured(2).toInt());
    return size.width() >= 2 && size.height() >= 2 && size.width() <= 16384 && size.height() <= 16384 ? size : QSize();
}
}

bool MotionAnalysis::add(const QImage &picture, QString *error) {
    const QImage rgba = picture.convertToFormat(QImage::Format_RGBA8888);
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_RGBA;
    frame->width = rgba.width();
    frame->height = rgba.height();
    frame->data[0] = const_cast<uint8_t *>(rgba.constBits());
    frame->linesize[0] = int(rgba.bytesPerLine());
    const bool ok = add(frame, error);
    av_frame_free(&frame);
    return ok;
}

bool MotionAnalysis::add(const AVFrame *picture, QString *error) {
    auto &s = *d;
    if (!s.file || !picture) return false;
    // A picture the GPU decoded (Vulkan Video, VA-API and the like, which the
    // editor uses where it can) is in the GPU's memory: brought down first,
    // for the analysis reads its planes.
    std::unique_ptr<AVFrame, void (*)(AVFrame *)> local(nullptr, [](AVFrame *f) { av_frame_free(&f); });
    if (picture->hw_frames_ctx) {
        local.reset(av_frame_alloc());
        if (!local || av_hwframe_transfer_data(local.get(), picture, 0) < 0) {
            if (error) *error = QCoreApplication::translate("Stabilise", "The picture cannot be prepared for the analysis.");
            return false;
        }
        picture = local.get();
    }
    s.scale = sws_getCachedContext(s.scale, picture->width, picture->height, AVPixelFormat(picture->format), s.size.width(),
                                   s.size.height(), AV_PIX_FMT_YUV444P, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!s.scale) {
        if (error) *error = QCoreApplication::translate("Stabilise", "The picture cannot be prepared for the analysis.");
        return false;
    }
    const int area = s.size.width() * s.size.height();
    uint8_t *out[3] = {s.planes.data(), s.planes.data() + area, s.planes.data() + 2 * area};
    const int outStride[3] = {s.size.width(), s.size.width(), s.size.width()};
    sws_scale(s.scale, picture->data, picture->linesize, 0, picture->height, out, outStride);
    VSFrame frame;
    vsFrameFillFromBuffer(&frame, s.planes.data(), &s.info);
    LocalMotions motions;
    if (vsMotionDetection(&s.detect, &motions, &frame) != VS_OK) {
        if (error) *error = QCoreApplication::translate("Stabilise", "The motion analysis failed.");
        return false;
    }
    const bool written = vsWriteToFile(&s.detect, s.file, &motions) == VS_OK;
    vs_vector_del(&motions);
    if (!written && error) *error = QCoreApplication::translate("Stabilise", "The stabilisation analysis file cannot be written.");
    return written;
}

bool MotionAnalysis::finish(QString *error) {
    auto &s = *d;
    if (!s.file) return false;
    const bool ok = std::fflush(s.file) == 0 && std::fclose(s.file) == 0;
    s.file = nullptr;
    if (!ok && error) *error = QCoreApplication::translate("Stabilise", "The stabilisation analysis file cannot be written.");
    return ok;
}

namespace {
std::atomic<int> parses{0};

// The most a frame's picture can be scaled (where the stabiliser's effect
// samples, 1 - zoom / 100) and still fill a
// w × h frame once turned by `alpha` about the centre and moved by (x, y):
// every pixel of the frame drawn wholly from the picture, half a pixel in
// from its edge (where its samples start to blend with the nothing beyond),
// at this size and at half of it, as a smaller viewer draws it.
// The frame's corner farthest out along each axis once turned decides it.
// (In the picture's own pixels, as the corrections and the effect are: a
// picture of pixels not square is analysed and drawn at the shape it is
// shown, so the turn is the same there.)
double fillingScale(double x, double y, double alpha, double w, double h) {
    const double c = std::abs(std::cos(alpha)), s = std::abs(std::sin(alpha));
    const double halfW = w / 2 - 0.5, halfH = h / 2 - 0.5;  // the frame's outermost pixel centres
    const double roomX = w / 2 - 1 - std::abs(x), roomY = h / 2 - 1 - std::abs(y);
    return std::min(roomX / (c * halfW + s * halfH), roomY / (s * halfW + c * halfH));
}
}  // namespace

void fillEveryFrame(Corrections &list, const QSize &seen) {
    const double w = seen.width(), h = seen.height();
    const auto zoomFor = [&](double share) {
        double scale = 1;
        for (const Correction &c : list) scale = std::min(scale, fillingScale(share * c.x, share * c.y, share * c.alpha, w, h));
        return 100 * (1 - scale);
    };
    double share = 1;
    if (zoomFor(1) > stabiliserMostZoom) {
        double low = 0, high = 1;
        for (int i = 0; i < 40; ++i) {
            const double middle = (low + high) / 2;
            if (zoomFor(middle) <= stabiliserMostZoom) low = middle;
            else high = middle;
        }
        share = low;
        for (Correction &c : list) { c.x *= share; c.y *= share; c.alpha *= share; }
    }
    const double zoom = std::clamp(zoomFor(1), 0., stabiliserMostZoom);
    for (Correction &c : list) c.zoom = zoom;
    list.zoom = zoom;
    list.steadied = share;
}
int stabilisationParses() { return parses.load(); }

std::shared_ptr<const Corrections> stabilisation(const QString &results, int smoothing, const QSize &frame, QString *error) {
    // What each analysis file gave, by file, smoothing and frame, while the
    // file stays as it was; a file that could not be read or turned into
    // corrections is remembered too, so it is not parsed again on every frame.
    struct Kept { QDateTime modified; std::shared_ptr<const Corrections> corrections; const char *failure = nullptr; };
    static QMutex mutex;
    static QHash<QString, Kept> kept;
    const QFileInfo info(results);
    const QDateTime modified = info.lastModified();
    const QString key = QStringLiteral("%1|%2|%3x%4").arg(results).arg(smoothing).arg(frame.width()).arg(frame.height());
    auto answer = [&](const Kept &k) {
        if (k.failure && error) *error = QCoreApplication::translate("Stabilise", k.failure);
        return k.corrections;
    };
    {
        QMutexLocker lock(&mutex);
        if (const auto it = kept.constFind(key); it != kept.constEnd() && it->modified == modified) return answer(*it);
    }
    // Read and worked out with the lock let go: a long clip's file takes a
    // while, and another renderer asking for another clip's does not wait.
    // (Two asking for the same new file both work it out; the answers agree.)
    Kept made;
    made.modified = modified;
    ++parses;
    [&] {
        FILE *file = std::fopen(QFile::encodeName(results).constData(), "r");
        if (!file) { made.failure = QT_TRANSLATE_NOOP("Stabilise", "The stabilisation analysis file cannot be read."); return; }
        VSManyLocalMotions motions;
        const int read = vsReadLocalMotionsFile(file, &motions);
        std::fclose(file);
        if (read != VS_OK) { made.failure = QT_TRANSLATE_NOOP("Stabilise", "The stabilisation analysis file is damaged."); return; }
        if (vs_vector_size(&motions) == 0) {
            // Analysed, but no motion found (a flat picture): nothing to correct.
            vs_vector_del(&motions);
            made.corrections = std::make_shared<const Corrections>();
            return;
        }
        // The frame the motions were found in: the picture the file names, or
        // (a file from before it did) the timeline's whole frame.
        const QSize picture = pictureOf(results);
        // (Made even: the library takes no odd size, and half a pixel of
        // frame is nothing to the corrections.)
        const QSize given = picture.isValid() ? picture : frame;
        const QSize seen(std::max(2, given.width() + given.width() % 2), std::max(2, given.height() + given.height() % 2));
        // As the clip's stabiliser has always been set up: relative motion,
        // smoothing as chosen, no limits, bilinear. Not the library's own
        // zoom (its "optimal static zoom" leaves out the turns and the
        // largest tenth of the moves, so a hard shake shows an edge on some
        // frames): the zoom is worked out after, to fill every frame.
        VSFrameInfo fi;
        vsFrameInfoInit(&fi, seen.width(), seen.height(), PF_YUV420P);
        VSTransformConfig conf = vsTransformGetDefaultConfig("omaedit");
        conf.smoothing = std::clamp(smoothing, 0, 100);
        conf.relative = 1;
        conf.crop = VSCropBorder;
        conf.invert = 0;
        conf.zoom = 0;
        conf.optZoom = 0;
        conf.zoomSpeed = 0.25;
        conf.maxShift = -1;
        conf.maxAngle = -1;
        conf.interpolType = VS_BiLinear;
        conf.verbose = 0;
        VSTransformData td;
        VSTransformations trans;
        vsTransformationsInit(&trans);
        if (vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK) {
            if (vsLocalmotions2Transforms(&td, &motions, &trans) == VS_OK && vsPreprocessTransforms(&td, &trans) == VS_OK) {
                auto list = std::make_shared<Corrections>();
                list->picture = picture;
                list->reserve(size_t(trans.len));
                for (int i = 0; i < trans.len; ++i) list->push_back({trans.ts[i].x, trans.ts[i].y, trans.ts[i].alpha, 0});
                fillEveryFrame(*list, seen);
                made.corrections = list;
            }
            vsTransformDataCleanup(&td);
        }
        vsTransformationsCleanup(&trans);
        for (int i = 0; i < vs_vector_size(&motions); ++i)
            if (auto *each = static_cast<VSVector *>(vs_vector_get(&motions, i))) vs_vector_del(each);
        vs_vector_del(&motions);
        if (!made.corrections) made.failure = QT_TRANSLATE_NOOP("Stabilise", "The stabilisation analysis could not be turned into corrections.");
    }();
    QMutexLocker lock(&mutex);
    if (kept.size() > 32) kept.clear();
    kept.insert(key, made);
    return answer(made);
}

}  // namespace render
