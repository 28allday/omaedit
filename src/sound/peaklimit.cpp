#include "peaklimit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sound {

namespace {

// BS.1770-4 Annex 2's 4× interpolator (as LoudnessMeter uses): four 12-tap phases.
constexpr int kPhases = 4;
constexpr int kTaps = 12;
const double kCoeffs[kPhases][kTaps] = {
    {0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000,
     -0.0594482421875, 0.1373291015625, 0.9721679687500, -0.1022949218750,
     0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500},
    {-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250,
     -0.1665039062500, 0.4650878906250, 0.7797851562500, -0.2003173828125,
     0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375},
    {-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000,
     -0.2003173828125, 0.7797851562500, 0.4650878906250, -0.1665039062500,
     0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875},
    {-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750,
     -0.1022949218750, 0.9721679687500, 0.1373291015625, -0.0594482421875,
     0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750}};

// The gain wobbling under a peak lifts the reconstruction a little; aim
// this far under the ceiling.
constexpr double kMarginDb = 0.2;

// ReconstructedPeak's interpolator: a 4× windowed sinc (Kaiser, beta 6),
// 32 inputs a phase, each phase scaled to unity gain. Phase 0 is the
// input sample itself (the sinc's zeros fall on the other inputs).
constexpr int kLongTaps = 32;
// The input each set of points follows: 15 inputs before it, 16 after.
constexpr int kCentre = kLongTaps / 2 - 1;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

struct LongInterpolator {
    // phase p (1..3) at k: the weight of input (centre − 15 + k) for the
    // point p/4 of a sample after the centre.
    float coeffs[kPhases][kLongTaps] = {};
    LongInterpolator() {
        const double beta = 6.0, half = kLongTaps * kPhases / 2.0;
        for (int p = 1; p < kPhases; ++p) {
            double sum = 0.0, weights[kLongTaps];
            for (int k = 0; k < kLongTaps; ++k) {
                // The distance, in input samples, from this input to the point.
                const double t = double(kCentre - k) + double(p) / kPhases;
                const double x = M_PI * t;
                const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(x) / x;
                const double r = t * kPhases / half;
                const double window = std::abs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / besselI0(beta);
                weights[k] = sinc * window;
                sum += weights[k];
            }
            for (int k = 0; k < kLongTaps; ++k) coeffs[p][k] = float(weights[k] / sum);
        }
    }
};

const LongInterpolator &longInterpolator() {
    static const LongInterpolator table;
    return table;
}

}  // namespace

void ReconstructedPeak::reset(int channels) {
    m_channels = std::clamp(channels, 1, 8);
    // Each input is kept twice, kLongTaps apart, so the interpolator's
    // inputs are always in a row (oldest first) from m_pos.
    m_history.assign(size_t(m_channels), std::vector<float>(2 * kLongTaps, 0.0f));
    m_pos = 0;
    m_count = 0;
    m_peak = 0.0;
}

// The loudest of the centre sample and the three points after it, `w` the
// interpolator's inputs, oldest first.
double ReconstructedPeak::readings(const float *w) {
    const LongInterpolator &f = longInterpolator();
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int k = 0; k < kLongTaps; ++k) {
        a1 += f.coeffs[1][k] * w[k];
        a2 += f.coeffs[2][k] * w[k];
        a3 += f.coeffs[3][k] * w[k];
    }
    return double(std::max({std::fabs(w[kCentre]), std::fabs(a1), std::fabs(a2), std::fabs(a3)}));
}

void ReconstructedPeak::push(const float *in, int frames) {
    const size_t ch = size_t(m_channels);
    for (int i = 0; i < frames; ++i) {
        for (size_t c = 0; c < ch; ++c) {
            std::vector<float> &h = m_history[c];
            h[size_t(m_pos)] = h[size_t(m_pos + kLongTaps)] = in[size_t(i) * ch + c];
        }
        m_pos = (m_pos + 1) % kLongTaps;
        ++m_count;
        // Each new input completes the points after the sample half the
        // interpolator back.
        if (m_count > kCentre)
            for (size_t c = 0; c < ch; ++c)
                m_peak = std::max(m_peak, readings(m_history[c].data() + m_pos));
    }
}

double ReconstructedPeak::peakDb() const {
    // The last half-interpolator of samples, followed by silence.
    double peak = m_peak;
    for (const std::vector<float> &kept : m_history) {
        std::vector<float> h = kept;
        int pos = m_pos;
        for (int i = 0; i <= kCentre; ++i) {
            h[size_t(pos)] = h[size_t(pos + kLongTaps)] = 0.0f;
            pos = (pos + 1) % kLongTaps;
            if (m_count + i >= kCentre) peak = std::max(peak, readings(h.data() + pos));
        }
    }
    return peak <= 0.0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(peak);
}

void TruePeakLimiter::reset(int rate, int channels, double gainDb, double ceilingDb, double releaseMs) {
    rate = std::clamp(rate, 8000, 384000);
    m_channels = std::clamp(channels, 1, 8);
    m_gain = std::pow(10.0, gainDb / 20.0);
    m_ceiling = std::pow(10.0, (ceilingDb - kMarginDb) / 20.0);
    m_release = 1.0 - std::exp(-1.0 / (std::max(1.0, releaseMs) * 0.001 * rate));
    // 1.5 ms to reach a peak. A detector reading at sample n covers inputs
    // n-11..n, so the hold reaches 11 samples further and the sound waits
    // for both (see process()).
    m_look = std::max(1, int(std::lround(0.0015 * rate)));
    m_hold = m_look + kTaps;
    m_delay = m_look + kTaps - 1;
    m_n = 0;
    m_history.assign(size_t(m_channels), std::vector<float>(kTaps, 0.0f));
    m_line.assign(size_t(m_delay + 1) * size_t(m_channels), 0.0f);
    m_linePos = 0;
    m_minimum.clear();
    m_box.assign(size_t(m_look), 1.0);
    m_boxPos = 0;
    m_boxSum = double(m_look);
    m_last = 1.0;
    m_lowest = 1.0;
}

void TruePeakLimiter::process(const float *in, int frames, float *out) {
    const size_t ch = size_t(m_channels);
    const int ring = m_delay + 1;
    for (int i = 0; i < frames; ++i, ++m_n) {
        // The detector: the loudest reconstructed peak of the gained input.
        double peak = 0.0;
        for (size_t c = 0; c < ch; ++c) {
            const float x = float(double(in[size_t(i) * ch + c]) * m_gain);
            std::vector<float> &h = m_history[c];
            std::rotate(h.rbegin(), h.rbegin() + 1, h.rend());
            h[0] = x;
            peak = std::max(peak, std::fabs(double(x)));
            for (int p = 0; p < kPhases; ++p) {
                double acc = 0.0;
                for (int k = 0; k < kTaps; ++k)
                    acc += kCoeffs[p][k] * double(h[size_t(k)]);
                peak = std::max(peak, std::fabs(acc));
            }
            m_line[size_t(m_linePos) * ch + c] = x;
        }
        const double need = peak > m_ceiling ? m_ceiling / peak : 1.0;
        // Hold: the lowest need over the last m_hold readings.
        while (!m_minimum.empty() && m_minimum.back().second >= need)
            m_minimum.pop_back();
        m_minimum.emplace_back(m_n, need);
        while (m_minimum.front().first <= m_n - m_hold)
            m_minimum.pop_front();
        const double held = m_minimum.front().second;
        // Smooth: the hold averaged over the look-ahead, so the gain glides
        // down to each peak and is there when the peak leaves the delay.
        m_boxSum += held - m_box[size_t(m_boxPos)];
        m_box[size_t(m_boxPos)] = held;
        m_boxPos = (m_boxPos + 1) % m_look;
        const double smooth = std::min(1.0, m_boxSum / m_look);
        // Release: down at once, up slowly.
        m_last = smooth < m_last ? smooth : m_last + (smooth - m_last) * m_release;
        m_lowest = std::min(m_lowest, m_last);
        // Out: the sample that went in m_delay samples ago.
        const int from = (m_linePos + 1) % ring;
        for (size_t c = 0; c < ch; ++c)
            out[size_t(i) * ch + c] = float(double(m_line[size_t(from) * ch + c]) * m_last);
        m_linePos = (m_linePos + 1) % ring;
    }
}

double TruePeakLimiter::deepestReduction() const {
    return m_lowest >= 1.0 ? 0.0 : 20.0 * std::log10(m_lowest);
}

}  // namespace sound
