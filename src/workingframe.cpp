#include "workingframe.h"
#include "colourpipeline.h"
#include "bands.h"
#include "imagebands.h"

#include <QColorSpace>
#include <atomic>
#include <cmath>
#include <cstring>

namespace {
WorkingFrame failure(const QString &message, QString *error) {
    if (error) *error = message;
    return {};
}

WorkingFrame blend(const WorkingFrame &a, const WorkingFrame &b, double amount,
                   bool sourceOver, QString *error) {
    if (error) error->clear();
    QString problem = a.validate();
    if (problem.isEmpty()) problem = b.validate();
    if (!problem.isEmpty()) return failure(problem, error);
    if (a.config() != b.config() || a.configHash() != b.configHash() || a.space() != b.space())
        return failure(QStringLiteral("Cannot blend working frames with different colour identities."), error);
    if (a.pixels().size() != b.pixels().size())
        return failure(QStringLiteral("Working-frame blend requires matching dimensions."), error);
    if (!std::isfinite(amount) || amount < 0 || amount > 1)
        return failure(QStringLiteral("Working-frame blend amount must be between zero and one."), error);
    if (amount == 0) return a;
    if (!sourceOver && amount == 1) return b;

    QImage result(a.pixels().size(), QImage::Format_RGBA32FPx4);
    if (result.isNull()) return failure(QStringLiteral("Cannot allocate a working frame."), error);
    for (int y = 0; y < result.height(); ++y) {
        const float *p = reinterpret_cast<const float *>(a.pixels().constScanLine(y));
        const float *q = reinterpret_cast<const float *>(b.pixels().constScanLine(y));
        float *out = reinterpret_cast<float *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x, p += 4, q += 4, out += 4) {
            const double front = q[3] * amount;
            const double back = p[3] * (sourceOver ? 1 - front : 1 - amount);
            const double alpha = front + back;
            for (int c = 0; c < 3; ++c)
                out[c] = alpha > 0 ? (q[c] * front + p[c] * back) / alpha : 0;
            out[3] = alpha;
        }
    }
    return a.withPixels(result, error);
}
} // namespace

QString WorkingFrame::validate() const {
    if (m_pixels.isNull()) return QStringLiteral("Working frame is empty.");
    if (m_pixels.format() != QImage::Format_RGBA32FPx4)
        return QStringLiteral("Working frames require straight RGBA32F pixels.");
    const auto info = ColourPipeline::info();
    if (!info.value("error").toString().isEmpty()) return info.value("error").toString();
    if (m_config != info.value("config") || m_configHash != info.value("configHash"))
        return QStringLiteral("Working-frame OCIO configuration does not match the bundled configuration.");
    if (m_space != "oma_interchange")
        return QStringLiteral("Working frames require the linear Rec.709 interchange space.");
    const auto aspect=m_pixels.text("omaedit.pixelAspect");
    if(!aspect.isEmpty() && (!std::isfinite(aspect.toDouble()) || aspect.toDouble()<1./32 || aspect.toDouble()>32))
        return QStringLiteral("Working-frame pixel aspect is invalid.");
    return {};
}

namespace {
// Every pixel's colour finite and its coverage in [0, 1], without clamping
// negative RGB or HDR highlights: checked in bands on every core.
bool wellFormed(const QImage &image) {
    const int rows = Bands::rowsPerBand(image.width(), 16), bands = (image.height() + rows - 1) / rows;
    return Bands::run(bands, [&](int band) {
        for (int y = band * rows, end = qMin(image.height(), y + rows); y < end; ++y) {
            const float *p = reinterpret_cast<const float *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x, p += 4)
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])
                    || !std::isfinite(p[3]) || p[3] < 0 || p[3] > 1)
                    return false;
        }
        return true;
    });
}
// The pixels wellFormed refuses, put right in place: a colour that is not a
// number becomes black, coverage comes inside [0, 1]. How many were changed.
qint64 mended(QImage &image) {
    std::atomic<qint64> count{0};
    const int rows = Bands::rowsPerBand(image.width(), 16), bands = (image.height() + rows - 1) / rows;
    uchar *bits = image.bits();
    const qsizetype stride = image.bytesPerLine();
    Bands::run(bands, [&](int band) {
        qint64 here = 0;
        for (int y = band * rows, end = qMin(image.height(), y + rows); y < end; ++y) {
            float *p = reinterpret_cast<float *>(bits + y * stride);
            for (int x = 0; x < image.width(); ++x, p += 4) {
                const bool colour = std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
                const bool coverage = std::isfinite(p[3]) && p[3] >= 0 && p[3] <= 1;
                if (colour && coverage) continue;
                if (!colour) p[0] = p[1] = p[2] = 0;
                if (!coverage) p[3] = std::isfinite(p[3]) ? std::clamp(p[3], 0.f, 1.f) : std::isnan(p[3]) ? 0.f : p[3] > 0 ? 1.f : 0.f;
                ++here;
            }
        }
        count += here;
        return true;
    });
    return count.load();
}
// A working frame's own pixels carry no colour space and no working tags
// (those belong to taggedImage()). Only what is there is cleared, so a
// picture nothing else holds is not copied.
void untag(QImage &pixels) {
    if (pixels.colorSpace().isValid()) pixels.setColorSpace(QColorSpace());
    for (const auto &key : pixels.textKeys())
        if (key.startsWith("omaedit.working.") && !pixels.text(key).isEmpty()) pixels.setText(key, QString());
}
}  // namespace

WorkingFrame WorkingFrame::fromLinear(const QImage &image, const QString &config,
                                     const QString &configHash, const QString &space,
                                     QString *error) {
    if (error) error->clear();
    WorkingFrame frame;
    frame.m_pixels = image;
    frame.m_config = config;
    frame.m_configHash = configHash;
    frame.m_space = space;
    const QString problem = frame.validate();
    if (!problem.isEmpty()) return failure(problem, error);
    if (!wellFormed(image))
        return failure(QStringLiteral("Working frame contains nonfinite pixels or invalid alpha."), error);
    // QImage may wrap an externally owned decode buffer. Make ownership real
    // before that producer/frame is released or reuses its memory: copied in
    // row bands on every core, its metadata with it.
    QImage owned(image.size(), image.format());
    if (owned.isNull()) return failure(QStringLiteral("Cannot allocate a working frame."), error);
    uchar *const to = owned.bits();
    const qsizetype stride = owned.bytesPerLine(), bytes = qsizetype(image.width()) * 16;
    const int rows = Bands::rowsPerBand(image.width(), 16), bands = (image.height() + rows - 1) / rows;
    Bands::run(bands, [&](int band) {
        for (int y = band * rows, end = qMin(image.height(), y + rows); y < end; ++y)
            std::memcpy(to + y * stride, image.constScanLine(y), size_t(bytes));
        return true;
    });
    Bands::copyMetadata(owned, image);
    frame.m_pixels = std::move(owned);
    untag(frame.m_pixels);
    return frame;
}

WorkingFrame WorkingFrame::adoptLinear(QImage image, const QString &config,
                                      const QString &configHash, const QString &space,
                                      QString *error, bool mend) {
    if (error) error->clear();
    WorkingFrame frame;
    frame.m_pixels = std::move(image);
    frame.m_config = config;
    frame.m_configHash = configHash;
    frame.m_space = space;
    const QString problem = frame.validate();
    if (!problem.isEmpty()) return failure(problem, error);
    if (!wellFormed(frame.m_pixels)) {
        if (!mend) return failure(QStringLiteral("Working frame contains nonfinite pixels or invalid alpha."), error);
        const qint64 changed = mended(frame.m_pixels);
        // Said once in a while, not for every frame of a clip that has them.
        static std::atomic<int> said{0};
        if (said.fetch_add(1) % 250 == 0)
            qWarning("A drawn frame held %lld pixels that were not numbers or had coverage outside 0 to 1; they were made black.", static_cast<long long>(changed));
    }
    untag(frame.m_pixels);
    return frame;
}

WorkingFrame WorkingFrame::fromInput(const QImage &image, const QVariantMap &management,
                                    QString *error) {
    if (error) error->clear();
    if (!management.value("enabled").toBool() || management.value("engine") != "ocio")
        return failure(QStringLiteral("Working-frame input requires explicitly enabled OpenColorIO management."), error);
    const QString problem = ColourPipeline::validate(management);
    if (!problem.isEmpty()) return failure(problem, error);
    const auto pixels = ColourPipeline::convert(image,
        image.text("omaedit.inputLinearRec709") == "1" ? "oma_interchange" : ColourPipeline::spaceName(management.value("input").toString(),
                                  management.value("inputGamma").toString()),
        "oma_interchange", error);
    if (pixels.isNull()) {
        if (error && error->isEmpty()) *error = QStringLiteral("Working-frame input is empty.");
        return {};
    }
    return fromLinear(pixels, management.value("config").toString(),
                      management.value("configHash").toString(), "oma_interchange", error);
}

WorkingFrame WorkingFrame::withPixels(const QImage &image, QString *error) const {
    const QString problem = validate();
    if (!problem.isEmpty()) return failure(problem, error);
    // Only reuse this frame's already-owned, validated QImage. A new or
    // externally backed image still crosses the full validation/copy boundary.
    if (image.cacheKey() == m_pixels.cacheKey()) {
        if (error) error->clear();
        return *this;
    }
    auto pixels=image;
    if(pixels.text("omaedit.pixelAspect").isEmpty() && !m_pixels.text("omaedit.pixelAspect").isEmpty())
        pixels.setText("omaedit.pixelAspect",m_pixels.text("omaedit.pixelAspect"));
    return fromLinear(pixels, m_config, m_configHash, m_space, error);
}

QImage WorkingFrame::output(const QString &space, QString *error) const {
    const QString problem = validate();
    if (!problem.isEmpty()) { failure(problem, error); return {}; }
    return ColourPipeline::convert(m_pixels, m_space, space, error);
}

QImage WorkingFrame::display(const QVariantMap &options, QString *error, QImage::Format target) const {
    const QString problem = validate();
    if (!problem.isEmpty()) { failure(problem, error); return {}; }
    auto view = options;
    // Input interpretation belongs to the frame, never to viewer preferences.
    view["from"] = "rec709";
    view["fromGamma"] = "linear";
    return ColourPipeline::display(m_pixels, view, error, target);
}

WorkingFrame WorkingFrame::over(const WorkingFrame &background, const WorkingFrame &foreground,
                               double opacity, QString *error) {
    return blend(background, foreground, opacity, true, error);
}

WorkingFrame WorkingFrame::dissolve(const WorkingFrame &a, const WorkingFrame &b,
                                   double progress, QString *error) {
    return blend(a, b, progress, false, error);
}
