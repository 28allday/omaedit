#pragma once
#include <QVariantMap>
#include <QVector>
#include <optional>

// A compact mapping from output-frame boundaries to source-frame boundaries.
// Source coordinates use the timeline rate after media frame-rate interpretation.
// Cuts retain their original in/out in this mapping's output frame space.
class RetimeMap {
public:
    struct Sample {
        bool valid = false;
        double source = 0;
        double nextSource = 0;
        double speed = 0; // instantaneous rate at the beginning of this frame
        int segment = -1;
        bool holdBefore = false;
    };
    bool set(const QVariantMap &definition, QString *error = nullptr);
    // Full-source constant timing used by explicit recorded-picture conform.
    // Without an origin, retain existing rounded duration. A physical origin
    // (seconds from the playback-side source boundary, including zero) retains the final partial
    // interval so a marked last picture remains addressable. Source coordinates
    // stay absolute; both renderers use the same definition.
    static QVariantMap conform(int nativeFrames, double nativeFps, double timelineFps,
                               double speed = 1., double interpretation = 1.,
                               std::optional<double> originSeconds = {});
    bool isValid() const { return !m_segments.isEmpty(); }
    int frames() const { return m_frames; }
    double sourceLength() const { return m_sourceLength; }
    QPair<double,double> sourceBounds() const { return {m_sourceFirst,m_sourceEnd}; }
    QVariantMap definition() const { return m_definition; }
    Sample sample(int frame) const;
    // The source position at a fractional output frame (0 to frames()), and
    // the rate there: what a sample-accurate reader needs between frames.
    double sourceAt(double frame, double *rate = nullptr) const;
    // Some segment changes speed across its span (a ramp, not a constant).
    bool hasRamp() const;
    // Legacy mapped sampling: floor at the forward start boundary and just
    // before the reverse boundary. Retained for existing saved mode-0 clips.
    int sourceFrame(int frame, double timelineFps, double sourceFps) const;
    struct ImageSample {
        int first = -1, second = -1;
        double mix = 0; // contribution of second; both indices are bounded
    };
    // Fractional picture sampling. Reverse boundaries name the picture one
    // frame before them; held reverse samples retain that same convention.
    ImageSample imageSample(int frame, double timelineFps, double sourceFps) const;
    // Explicit Floor/Nearest and interpolation share recorded picture positions.
    // Original preserves the historical boundary convention above.
    ImageSample pictureSample(int frame, double timelineFps, double sourceFps, int mode) const;
    QVariantMap blade(int frame, QString *error = nullptr) const;
    // Return a local map for an output window, retaining the original curve
    // phases and intentional source jumps. Source coordinates stay absolute.
    QVariantMap window(int first, int count, QString *error = nullptr) const;
    // Translate into another source's coordinates. The required output window
    // must remain covered; retain every contiguous valid trim handle around it.
    // windowFirst receives the kept window's start in this map's output space.
    // A failure leaves both this map and windowFirst unchanged.
    QVariantMap retarget(double sourceOffset, double sourceLength, int first, int count,
                         int *windowFirst, QString *error = nullptr) const;

private:
    struct Segment {
        int start = 0, frames = 0;
        double source = 0, speed = 1, endSpeed = 1, curveStart = 0, curveEnd = 1;
        bool smooth = false, holdBefore = false;
        double value(double u) const;
        double position(double offset) const;
        double rate(double offset) const;
    };
    QVector<Segment> m_segments;
    QVariantMap m_definition;
    int m_frames = 0;
    double m_sourceLength = 0;
    double m_sourceFirst = 0, m_sourceEnd = 0;
};
