#pragma once

// A look-ahead limiter on the true peak: the delivered sound levelled by a
// fixed gain, with every reconstructed peak (BS.1770's 4× oversampling, the
// loudness meter's own interpolator) held at or under a ceiling. The gain
// reaches each peak before it arrives and lets go slowly after, so loud
// passages are not pumped and quiet ones are untouched.
//
// The output trails the input by latency() samples: feed the first
// latency() samples ahead of time and drop what comes out, then each block
// in returns the block that started latency() samples earlier.

#include <deque>
#include <vector>

namespace sound {

class TruePeakLimiter {
public:
    // channels 1..8; gainDb applied to everything, ceilingDb in dBTP.
    void reset(int rate, int channels, double gainDb, double ceilingDb, double releaseMs = 80.0);
    int latency() const { return m_delay; }
    // Interleaved, `frames` sample frames; `out` may not alias `in`.
    void process(const float *in, int frames, float *out);
    // The deepest gain reduction so far, dB (0: never limited).
    double deepestReduction() const;

private:
    int m_channels = 2;
    double m_gain = 1.0, m_ceiling = 1.0, m_release = 0.0;
    int m_look = 0, m_hold = 0, m_delay = 0;
    long long m_n = 0;
    std::vector<std::vector<float>> m_history;  // per channel, the interpolator's last 12 inputs
    std::vector<float> m_line;                  // the delayed, gained input, interleaved ring
    int m_linePos = 0;
    std::deque<std::pair<long long, double>> m_minimum;  // the hold window's running minimum
    std::vector<double> m_box;                  // the smoothing window
    int m_boxPos = 0;
    double m_boxSum = 0.0, m_last = 1.0, m_lowest = 1.0;
};

// The true peak a careful reconstruction finds: the sound 4× oversampled
// through a long windowed-sinc interpolator (32 taps a phase, flat to about
// 21 kHz at 48 kHz), about as FFmpeg's EBU R128 meter resamples for its
// reading. BS.1770's own 12-tap interpolator (the limiter's and the
// loudness meter's) can read a broadband transient up to a decibel lower;
// a delivery's sound is checked with the larger of the two readings.
class ReconstructedPeak {
public:
    // channels 1..8.
    void reset(int channels);
    // Interleaved, `frames` sample frames.
    void push(const float *in, int frames);
    // The loudest sample or point between samples so far, dBTP (the last
    // samples given are read as if silence followed); -inf for silence.
    double peakDb() const;

private:
    int m_channels = 2;
    std::vector<std::vector<float>> m_history;  // per channel, the last 32 inputs (twice over)
    int m_pos = 0;
    long long m_count = 0;
    double m_peak = 0.0;
    static double readings(const float *w);
};

}  // namespace sound
