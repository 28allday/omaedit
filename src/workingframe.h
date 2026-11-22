#pragma once

#include <QImage>
#include <QMetaType>
#include <QVariantMap>

// A scene-linear image, never an encoded preview. Pixels are owned, implicitly
// shared straight RGBA32F in the suite's linear Rec.709 D65 interchange space.
// Only explicit input/output/display boundaries perform colour transforms.
class WorkingFrame {
public:
    enum class Alpha { Straight };

    WorkingFrame() = default;
    static WorkingFrame fromInput(const QImage &image, const QVariantMap &management,
                                  QString *error = nullptr);
    // For float processing results and cache readers: do not promote integer
    // images and pretend their precision or colour interpretation is known.
    static WorkingFrame fromLinear(const QImage &image, const QString &config,
                                   const QString &configHash, const QString &space,
                                   QString *error = nullptr);
    // The same for an image that owns its pixels and nothing else holds
    // (a fresh readback): checked, and kept without a copy.
    // `mend`: a pixel whose colour is not a number (or whose coverage is
    // outside 0 to 1) is made black (its coverage brought inside) rather
    // than the whole frame refused: for a frame the engine drew, where one
    // such pixel must not stop a paused frame or an export that playback
    // shows without a second look.
    static WorkingFrame adoptLinear(QImage image, const QString &config,
                                    const QString &configHash, const QString &space,
                                    QString *error = nullptr, bool mend = false);

    bool isNull() const { return m_pixels.isNull(); }
    const QImage &pixels() const { return m_pixels; }
    QString config() const { return m_config; }
    QString configHash() const { return m_configHash; }
    QString space() const { return m_space; }
    Alpha alpha() const { return Alpha::Straight; }
    QString validate() const;

    WorkingFrame withPixels(const QImage &image, QString *error = nullptr) const;
    QImage output(const QString &space, QString *error = nullptr) const;
    QImage display(const QVariantMap &options, QString *error = nullptr,
                   QImage::Format target = QImage::Format_RGBA32FPx4) const;
    QImage taggedImage() const;
    static bool isTagged(const QImage &image);
    static WorkingFrame fromTagged(const QImage &image, QString *error = nullptr);
    // A picture as the default display shows it, 8-bit: a tagged working
    // frame through the display transform, anything else as it is.
    static QImage shown(const QImage &image);
    bool save(const QString &path, const QVariantMap &metadata = {}, QString *error = nullptr) const;
    static WorkingFrame load(const QString &path, QVariantMap *metadata = nullptr, QString *error = nullptr);

    // Float linear primitives shared by the timeline compositor.
    // Coverage is premultiplied for blending, then returned as straight alpha.
    static WorkingFrame over(const WorkingFrame &background, const WorkingFrame &foreground,
                             double opacity = 1, QString *error = nullptr);
    static WorkingFrame dissolve(const WorkingFrame &a, const WorkingFrame &b,
                                 double progress, QString *error = nullptr);

private:
    QImage m_pixels;
    QString m_config, m_configHash, m_space;
};

Q_DECLARE_METATYPE(WorkingFrame)
