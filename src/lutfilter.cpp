#include "lutfilter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace {
std::atomic<quint64> generation{0};
QString stamp(const QString &path) {
    const QFileInfo file(path);
    return file.absoluteFilePath() + QLatin1Char('|') + QString::number(file.size())
        + QLatin1Char('|') + QString::number(file.lastModified().toMSecsSinceEpoch())
        + QLatin1Char('|') + QString::number(generation.load());
}

class Graph {
public:
    ~Graph() { avfilter_graph_free(&graph); }
    bool open(const QString &path, int w, int h, bool wide, QString *error) {
        const QString key = stamp(path) + QStringLiteral("|%1x%2:%3").arg(w).arg(h).arg(wide);
        if (graph && key == identity) return true;
        avfilter_graph_free(&graph);
        input = output = nullptr;
        identity.clear();
        if (!avfilter_get_by_name("buffer") || !avfilter_get_by_name("lut3d") || !avfilter_get_by_name("buffersink")) {
            *error = QCoreApplication::translate("LutFilter", "This FFmpeg build does not provide the 3D LUT filter."); return false;
        }
        graph = avfilter_graph_alloc();
        if (!graph) { *error = QCoreApplication::translate("LutFilter", "Not enough memory to load the LUT."); return false; }
        graph->nb_threads = 1;
        const AVPixelFormat format = wide ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGBA;
        const QByteArray args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=1/25:pixel_aspect=1/1")
            .arg(w).arg(h).arg(int(format)).toLatin1();
        int result = avfilter_graph_create_filter(&input, avfilter_get_by_name("buffer"), "input", args.constData(), nullptr, graph);
        AVFilterContext *lut = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("lut3d"), "lut");
        if (result >= 0 && !lut) result = AVERROR(ENOMEM);
        if (result >= 0) result = av_opt_set(lut, "file", path.toUtf8().constData(), AV_OPT_SEARCH_CHILDREN);
        if (result >= 0) result = avfilter_init_str(lut, nullptr);
        if (result >= 0) result = avfilter_graph_create_filter(&output, avfilter_get_by_name("buffersink"), "output", nullptr, nullptr, graph);
        if (result >= 0) result = avfilter_link(input, 0, lut, 0);
        if (result >= 0) result = avfilter_link(lut, 0, output, 0);
        if (result >= 0) result = avfilter_graph_config(graph, nullptr);
        if (result < 0) {
            char message[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(result, message, sizeof(message));
            *error = QCoreApplication::translate("LutFilter", "Could not read this 3D LUT: %1").arg(QString::fromUtf8(message));
            avfilter_graph_free(&graph);
            return false;
        }
        identity = key;
        serial = 0;
        return true;
    }

    bool apply(uint8_t *pixels, int w, int h, bool wide, double amount) {
        AVFrame *in = av_frame_alloc(), *out = av_frame_alloc();
        if (!in || !out) { av_frame_free(&in); av_frame_free(&out); return false; }
        in->format = wide ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGBA;
        in->width = w; in->height = h; in->pts = serial++;
        int result = av_frame_get_buffer(in, 0);
        const int rowBytes = w * (wide ? 8 : 4);
        if (result >= 0) {
            for (int y = 0; y < h; ++y) std::memcpy(in->data[0] + y * in->linesize[0], pixels + y * rowBytes, rowBytes);
            result = av_buffersrc_add_frame_flags(input, in, AV_BUFFERSRC_FLAG_KEEP_REF);
        }
        if (result >= 0) result = av_buffersink_get_frame(output, out);
        if (result >= 0 && (out->format != in->format || out->width != w || out->height != h)) result = AVERROR(EINVAL);
        if (result >= 0) {
            for (int y = 0; y < h; ++y) {
                uint8_t *original = pixels + y * rowBytes;
                const uint8_t *graded = out->data[0] + y * out->linesize[0];
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const int offset = (x * 4 + c) * (wide ? 2 : 1);
                        if (wide) {
                            const quint16 a = qFromUnaligned<quint16>(original + offset);
                            const quint16 b = qFromUnaligned<quint16>(graded + offset);
                            qToUnaligned<quint16>(quint16(std::lround(a + amount * (double(b) - a))), original + offset);
                        } else {
                            original[offset] = uint8_t(std::lround(original[offset] + amount * (double(graded[offset]) - original[offset])));
                        }
                    }
                    // Alpha belongs to the clip's compositing/mask, not its LUT.
                }
            }
        }
        av_frame_free(&in); av_frame_free(&out);
        return result >= 0;
    }

private:
    AVFilterGraph *graph = nullptr;
    AVFilterContext *input = nullptr, *output = nullptr;
    QString identity;
    qint64 serial = 0;
};

QMutex validationMutex;
QHash<QString, QString> validation;
}

bool LutFilter::apply(QImage &image, const QString &path, double intensity, QString *error) {
    const double amount = std::isfinite(intensity) ? std::clamp(intensity, 0.0, 1.0) : 1.0;
    if (amount <= 0.0 || image.isNull()) return true;
    // Looked up on 16-bit RGBA, as the grade works.
    image = image.convertToFormat(QImage::Format_RGBA64);
    thread_local Graph graph;
    QString problem;
    if (!graph.open(path, image.width(), image.height(), true, &problem)
        || !graph.apply(image.bits(), image.width(), image.height(), true, amount)) {
        if (error) *error = problem.isEmpty() ? QStringLiteral("Could not apply the LUT.") : problem;
        return false;
    }
    return true;
}

QString LutFilter::error(const QString &path) {
    const QFileInfo file(path);
    if (!file.exists()) return QCoreApplication::translate("LutFilter", "LUT file is missing. Locate its replacement.");
    if (!file.isFile() || !file.isReadable()) return QCoreApplication::translate("LutFilter", "LUT file is not readable.");
    if (file.size() > 64 * 1024 * 1024) return QCoreApplication::translate("LutFilter", "LUT files larger than 64 MiB are not supported.");
    if (file.suffix().compare(QStringLiteral("cube"), Qt::CaseInsensitive)
        && file.suffix().compare(QStringLiteral("3dl"), Qt::CaseInsensitive))
        return QCoreApplication::translate("LutFilter", "Choose a .cube or .3dl 3D LUT.");
    const QString key = stamp(path);
    QMutexLocker lock(&validationMutex);
    if (validation.contains(key)) return validation.value(key);
    Graph graph;
    QString error;
    graph.open(path, 2, 2, false, &error);
    if (validation.size() >= 128) validation.clear();
    validation.insert(key, error);
    return error;
}

void LutFilter::clearValidationCache() {
    QMutexLocker lock(&validationMutex);
    validation.clear();
    ++generation;
}
