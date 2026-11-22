#pragma once

#include <QList>
#include <QVariantMap>

#include <cstdint>
#include <vector>

// EBU R128 / ITU-R BS.1770-4 loudness meter, in-process, sample-rate
// agnostic. Feed it interleaved audio as it plays (push) or as one offline
// pass; read momentary / short-term / integrated LUFS, loudness range (LU)
// and true peak (dBTP) any time. No dependency beyond the standard library.
//
// Maths (the parts an operator can check against a reference meter):
// - K-weighting = the spec's two biquads (high shelf +4 dB @ 1681 Hz, then
//   a 38 Hz high-pass), coefficients derived for the given rate the way
//   libebur128 does it, so 44.1/48/96 kHz all measure alike.
// - Momentary = 400 ms window, short-term = 3 s, both stepped every 100 ms.
// - Integrated = mean power of the 400 ms blocks above −70 LUFS absolute
//   and then above (that mean − 10 LU) relative.
// - LRA = 95th − 10th percentile of the short-term values above −70 LUFS
//   absolute and (mean − 20 LU) relative (EBU Tech 3342).
// - True peak = the spec's 4× oversampling polyphase FIR on the unweighted
//   signal; sample peak kept beside it.
// EBU Tech 3341's reference holds: a 997 Hz sine at −23 dBFS on both
// channels reads −23.0 LUFS (the K-weighting's gain at 1 kHz and the
// spec's −0.691 offset cancel).
class LoudnessMeter {
public:
    LoudnessMeter();

    // Start over for a given signal. channels 1..8; the surround weights
    // (1.41 for Ls/Rs in a 5.x layout) follow the BS.1770 channel order.
    void reset(int sampleRate = 48000, int channels = 2);
    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }

    // Interleaved samples, `frames` sample frames per channel.
    void push(const float *interleaved, int frames);
    void push(const int16_t *interleaved, int frames);

    // Readings. -HUGE_VAL-ish silence is reported as −100 (the meters'
    // convention in this app), never as −inf.
    double momentary() const;   // LUFS, last 400 ms
    double shortTerm() const;   // LUFS, last 3 s
    double integrated() const;  // LUFS, gated, whole programme so far
    double range() const;       // LU
    double truePeak() const;    // dBTP, max so far
    double samplePeak() const;  // dBFS, max so far
    double momentaryMax() const { return m_momentaryMax; }
    double shortTermMax() const { return m_shortTermMax; }
    // Seconds of audio measured so far.
    double seconds() const;
    // Momentary values, one per 100 ms hop, oldest first (capped — the
    // last `historyLimit` hops are kept).
    QList<float> history() const;
    void setHistoryLimit(int hops) { m_historyLimit = hops; }

    // Everything above as one map — what the UI binds to.
    QVariantMap snapshot() const;

    // dB of gain that would bring `integrated` onto `targetLufs`.
    double gainToTarget(double targetLufs) const;

    static double silence() { return -100.0; }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        inline double run(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void designFilters();
    void endHop();
    static double toLufs(double power);
    static double gatedMean(const std::vector<double> &blocks, const std::vector<double> &lufs, double relativeGateLu,
                            double *outCount = nullptr);

    int m_rate = 48000;
    int m_channels = 2;
    std::vector<double> m_weights;          // per channel (1 or 1.41)
    std::vector<Biquad> m_shelf, m_highpass; // per channel
    // Running 100 ms hop accumulation of z_i (mean square of weighted signal).
    int m_hopFrames = 4800;
    int m_hopFill = 0;
    std::vector<double> m_hopSum;           // per channel
    // Per-hop powers (sum over channels of G_i * z_i), whole programme.
    std::vector<double> m_hopPowers;
    // Blocks: momentary = 4 hops, short-term = 30 hops.
    std::vector<double> m_momentaryBlocks;  // power per 400 ms block, every hop
    std::vector<double> m_shortTermBlocks;  // power per 3 s block, every hop
    // Each block's loudness in LUFS, worked out as it is made: a reading
    // gates and orders the blocks without taking a logarithm of each again.
    std::vector<double> m_momentaryLufs, m_shortTermLufs;
    std::vector<float> m_history;
    int m_historyLimit = 3000;
    double m_momentaryMax = -100.0;
    double m_shortTermMax = -100.0;
    // Peaks.
    double m_samplePeak = 0.0;
    double m_truePeak = 0.0;
    std::vector<std::vector<float>> m_tpHistory; // per channel, last 12 samples
};
