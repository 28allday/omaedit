#include "loudness.h"

#include <algorithm>
#include <cmath>

namespace {

// BS.1770-4 Annex 2: the 4× oversampling true-peak interpolator, 48 taps
// as four 12-tap phases.
constexpr int kTpPhases = 4;
constexpr int kTpTaps = 12;
const double kTpCoeffs[kTpPhases][kTpTaps] = {
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

constexpr int kMomentaryHops = 4;   // 400 ms
constexpr int kShortTermHops = 30;  // 3 s
constexpr double kAbsoluteGate = -70.0;

// The value `p` of the way through `values` in order (between two of them,
// in proportion), found without sorting them all: a live meter asks several
// times a second, over hours of blocks.
double percentile(std::vector<double> &values, double p) {
    if (values.empty())
        return LoudnessMeter::silence();
    const double pos = p * double(values.size() - 1);
    const size_t lo = size_t(std::floor(pos));
    const size_t hi = std::min(values.size() - 1, lo + 1);
    const double frac = pos - double(lo);
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(lo), values.end());
    const double low = values[lo];
    const double high = hi > lo ? *std::min_element(values.begin() + std::ptrdiff_t(hi), values.end()) : low;
    return low + (high - low) * frac;
}

} // namespace

LoudnessMeter::LoudnessMeter() { reset(); }

void LoudnessMeter::reset(int sampleRate, int channels) {
    m_rate = std::max(8000, sampleRate);
    m_channels = std::clamp(channels, 1, 8);
    m_weights.assign(size_t(m_channels), 1.0);
    // BS.1770 by FFmpeg's default layout for the count: surrounds weigh 1.41,
    // the LFE is not measured. 5.0 is L R C Ls Rs; 5.1, 6.1 and 7.1 are
    // L R C LFE then surrounds (back and side alike).
    if (m_channels == 5) {
        m_weights[3] = m_weights[4] = 1.41;
    } else if (m_channels >= 6) {
        m_weights[3] = 0.0;
        for (int c = 4; c < m_channels; ++c)
            m_weights[size_t(c)] = 1.41;
    }
    m_shelf.assign(size_t(m_channels), Biquad());
    m_highpass.assign(size_t(m_channels), Biquad());
    designFilters();
    m_hopFrames = std::max(1, m_rate / 10);
    m_hopFill = 0;
    m_hopSum.assign(size_t(m_channels), 0.0);
    m_hopPowers.clear();
    m_momentaryBlocks.clear();
    m_shortTermBlocks.clear();
    m_momentaryLufs.clear();
    m_shortTermLufs.clear();
    m_history.clear();
    m_momentaryMax = silence();
    m_shortTermMax = silence();
    m_samplePeak = 0.0;
    m_truePeak = 0.0;
    m_tpHistory.assign(size_t(m_channels), std::vector<float>(kTpTaps, 0.0f));
}

// The K-weighting pair, derived for the sample rate (libebur128's
// ebur128_init_filter algebra): a +4 dB high shelf at 1681 Hz then a 38 Hz
// second-order high-pass. At 48 kHz these land on the spec's table values.
void LoudnessMeter::designFilters() {
    double f0 = 1681.974450955533;
    const double G = 3.999843853973347;
    double Q = 0.7071752369554196;
    double K = std::tan(M_PI * f0 / double(m_rate));
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    Biquad shelf;
    shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf.b1 = 2.0 * (K * K - Vh) / a0;
    shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf.a1 = 2.0 * (K * K - 1.0) / a0;
    shelf.a2 = (1.0 - K / Q + K * K) / a0;

    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(M_PI * f0 / double(m_rate));
    a0 = 1.0 + K / Q + K * K;
    Biquad hp;
    hp.b0 = 1.0;
    hp.b1 = -2.0;
    hp.b2 = 1.0;
    hp.a1 = 2.0 * (K * K - 1.0) / a0;
    hp.a2 = (1.0 - K / Q + K * K) / a0;

    for (int c = 0; c < m_channels; ++c) {
        m_shelf[size_t(c)] = shelf;
        m_highpass[size_t(c)] = hp;
    }
}

void LoudnessMeter::push(const int16_t *interleaved, int frames) {
    if (!interleaved || frames <= 0)
        return;
    std::vector<float> buffer(size_t(frames) * size_t(m_channels));
    for (size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = float(interleaved[i]) / 32768.0f;
    push(buffer.data(), frames);
}

void LoudnessMeter::push(const float *interleaved, int frames) {
    if (!interleaved || frames <= 0)
        return;
    for (int n = 0; n < frames; ++n) {
        for (int c = 0; c < m_channels; ++c) {
            const float x = interleaved[size_t(n) * size_t(m_channels) + size_t(c)];
            // Peaks on the raw signal.
            const double ax = std::fabs(double(x));
            if (ax > m_samplePeak)
                m_samplePeak = ax;
            std::vector<float> &hist = m_tpHistory[size_t(c)];
            std::rotate(hist.rbegin(), hist.rbegin() + 1, hist.rend());
            hist[0] = x;
            for (int p = 0; p < kTpPhases; ++p) {
                double acc = 0.0;
                for (int k = 0; k < kTpTaps; ++k)
                    acc += kTpCoeffs[p][k] * double(hist[size_t(k)]);
                const double aa = std::fabs(acc);
                if (aa > m_truePeak)
                    m_truePeak = aa;
            }
            // K-weighted power.
            if (m_weights[size_t(c)] > 0.0) {
                const double w = m_highpass[size_t(c)].run(m_shelf[size_t(c)].run(double(x)));
                m_hopSum[size_t(c)] += w * w;
            }
        }
        if (++m_hopFill >= m_hopFrames)
            endHop();
    }
}

void LoudnessMeter::endHop() {
    double power = 0.0;
    for (int c = 0; c < m_channels; ++c) {
        power += m_weights[size_t(c)] * (m_hopSum[size_t(c)] / double(m_hopFrames));
        m_hopSum[size_t(c)] = 0.0;
    }
    m_hopFill = 0;
    m_hopPowers.push_back(power);
    const size_t hops = m_hopPowers.size();
    if (hops >= size_t(kMomentaryHops)) {
        double sum = 0.0;
        for (size_t i = hops - size_t(kMomentaryHops); i < hops; ++i)
            sum += m_hopPowers[i];
        const double block = sum / double(kMomentaryHops);
        m_momentaryBlocks.push_back(block);
        const double lufs = toLufs(block);
        m_momentaryLufs.push_back(lufs);
        m_momentaryMax = std::max(m_momentaryMax, lufs);
        m_history.push_back(float(lufs));
        if (int(m_history.size()) > m_historyLimit)
            m_history.erase(m_history.begin(),
                            m_history.begin() + (int(m_history.size()) - m_historyLimit));
    }
    if (hops >= size_t(kShortTermHops)) {
        double sum = 0.0;
        for (size_t i = hops - size_t(kShortTermHops); i < hops; ++i)
            sum += m_hopPowers[i];
        const double block = sum / double(kShortTermHops);
        m_shortTermBlocks.push_back(block);
        m_shortTermLufs.push_back(toLufs(block));
        m_shortTermMax = std::max(m_shortTermMax, m_shortTermLufs.back());
    }
}

double LoudnessMeter::toLufs(double power) {
    if (power <= 1e-12)
        return silence();
    return std::max(silence(), -0.691 + 10.0 * std::log10(power));
}

double LoudnessMeter::momentary() const {
    if (m_momentaryBlocks.empty())
        return silence();
    return toLufs(m_momentaryBlocks.back());
}

double LoudnessMeter::shortTerm() const {
    if (m_shortTermBlocks.empty())
        return silence();
    return toLufs(m_shortTermBlocks.back());
}

// Mean power of the blocks that pass the absolute gate and then the
// relative gate `relativeGateLu` below the absolute-gated mean. `lufs` holds
// each block's loudness, worked out once when the block was made.
double LoudnessMeter::gatedMean(const std::vector<double> &blocks, const std::vector<double> &lufs, double relativeGateLu,
                                double *outCount) {
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (lufs[i] > kAbsoluteGate) {
            sum += blocks[i];
            ++count;
        }
    }
    if (outCount)
        *outCount = 0;
    if (count == 0)
        return 0.0;
    const double relativeGate = toLufs(sum / count) - relativeGateLu;
    sum = 0.0;
    count = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const double l = lufs[i];
        if (l > kAbsoluteGate && l > relativeGate) {
            sum += blocks[i];
            ++count;
        }
    }
    if (outCount)
        *outCount = count;
    return count > 0 ? sum / count : 0.0;
}

double LoudnessMeter::integrated() const {
    double count = 0;
    const double mean = gatedMean(m_momentaryBlocks, m_momentaryLufs, 10.0, &count);
    return count > 0 ? toLufs(mean) : silence();
}

double LoudnessMeter::range() const {
    if (m_shortTermBlocks.empty())
        return 0.0;
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < m_shortTermBlocks.size(); ++i) {
        if (m_shortTermLufs[i] > kAbsoluteGate) {
            sum += m_shortTermBlocks[i];
            ++count;
        }
    }
    if (count == 0)
        return 0.0;
    const double relativeGate = toLufs(sum / count) - 20.0;
    std::vector<double> kept;
    kept.reserve(size_t(count));
    for (const double l : m_shortTermLufs) {
        if (l > kAbsoluteGate && l > relativeGate)
            kept.push_back(l);
    }
    if (kept.size() < 2)
        return 0.0;
    const double low = percentile(kept, 0.10);
    return std::max(0.0, percentile(kept, 0.95) - low);
}

double LoudnessMeter::truePeak() const {
    return m_truePeak <= 1e-6 ? silence() : 20.0 * std::log10(m_truePeak);
}

double LoudnessMeter::samplePeak() const {
    return m_samplePeak <= 1e-6 ? silence() : 20.0 * std::log10(m_samplePeak);
}

double LoudnessMeter::seconds() const {
    return double(m_hopPowers.size()) * 0.1 + double(m_hopFill) / double(m_rate);
}

QList<float> LoudnessMeter::history() const {
    return QList<float>(m_history.begin(), m_history.end());
}

QVariantMap LoudnessMeter::snapshot() const {
    QVariantMap map;
    map.insert(QStringLiteral("momentary"), momentary());
    map.insert(QStringLiteral("shortTerm"), shortTerm());
    map.insert(QStringLiteral("integrated"), integrated());
    map.insert(QStringLiteral("range"), range());
    map.insert(QStringLiteral("truePeak"), truePeak());
    map.insert(QStringLiteral("samplePeak"), samplePeak());
    map.insert(QStringLiteral("momentaryMax"), m_momentaryMax);
    map.insert(QStringLiteral("shortTermMax"), m_shortTermMax);
    map.insert(QStringLiteral("seconds"), seconds());
    return map;
}

double LoudnessMeter::gainToTarget(double targetLufs) const {
    const double i = integrated();
    if (i <= silence())
        return 0.0;
    return targetLufs - i;
}
