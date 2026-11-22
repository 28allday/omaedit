#pragma once
#include <QImage>
#include <functional>
#include <memory>

// Immutable bidirectional motion for one pair of pictures. Analysis retains no
// source pixels; callers own the pair and its media/colour/cache identity.
// Images must be straight RGBA8, RGBA16 or RGBA32F in the same colour space.
class MotionFlow {
public:
    using Cancel = std::function<bool()>;
    static std::shared_ptr<const MotionFlow> analyse(const QImage &first, const QImage &second,
        QString *error = nullptr, const Cancel &cancel = {});
    // Shared, bounded cache keyed by actual pixels, including alpha/low bits.
    // A file replacement or upstream colour change cannot reuse stale motion.
    // `madeFrom`: when the caller can say exactly what the pair was made from
    // (the file with its size and date, the two times, how it was read), that
    // stands for the pixels and they are not read to find the field again.
    // It must change whenever the pixels could: left empty, the pixels are the key.
    static std::shared_ptr<const MotionFlow> cached(const QImage &first, const QImage &second,
        QString *error = nullptr, const Cancel &cancel = {}, const QByteArray &madeFrom = {});
    static void clearCache();
    static qsizetype cacheBytes();
    static constexpr qsizetype cacheLimit() {return 96*1024*1024;}
    ~MotionFlow();
    // Cuts select the closer picture. Grossly unreliable motion blends the
    // original pair in premultiplied space, without inventing warped shapes.
    QImage interpolate(const QImage &first, const QImage &second, double phase,
        QString *error = nullptr, const Cancel &cancel = {}) const;
    bool sceneCut() const;
    QSize analysisSize() const;
    qsizetype memoryBytes() const;
private:
    MotionFlow();
    struct State;
    std::unique_ptr<State> d;
};
