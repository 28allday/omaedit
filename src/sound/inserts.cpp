#include "inserts.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/tx.h>
}

namespace sound {

namespace {

double number(const QVariantMap &m, const QString &key, double fallback = 0) {
    const double v = m.value(key, fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}
double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

// ------------------------------------------------------------------ biquads

// A second-order section with FFmpeg's af_biquads coefficients (width as a Q
// factor), run in direct form I in double, one memory per channel.
class Biquad : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Biquad>(*this); }
    enum class Kind { Equalizer, Bass, Treble, Lowpass, Highpass };
    Biquad(Kind kind, double frequency, double q, double gainDb) : m_kind(kind), m_frequency(frequency), m_q(q), m_gain(gainDb) {}
    void reset(int channels, int rate) override {
        m_memory.assign(size_t(channels) * 4, 0.);
        design(rate);
    }
    void process(float *data, int n, int channels, int rate) override {
        if (int(m_memory.size()) != channels * 4 || rate != m_rate) reset(channels, rate);
        if (m_bypass) return;
        for (int c = 0; c < channels; ++c) {
            double *m = &m_memory[size_t(c) * 4];  // x1 x2 y1 y2
            for (int i = 0; i < n; ++i) {
                const double x = data[size_t(i) * channels + c];
                const double y = m_b0 * x + m_b1 * m[0] + m_b2 * m[1] - m_a1 * m[2] - m_a2 * m[3];
                m[1] = m[0];
                m[0] = x;
                m[3] = m[2];
                m[2] = y;
                data[size_t(i) * channels + c] = float(y);
            }
        }
    }

private:
    void design(int rate) {
        m_rate = rate;
        const double A = std::pow(10., m_gain / 40);
        const double w0 = 2 * M_PI * m_frequency / rate;
        m_bypass = w0 > M_PI || w0 <= 0. || m_q <= 0.;
        if (m_bypass) return;
        const double alpha = std::sin(w0) / (2 * m_q);
        double beta;
        double a0 = 1, a1 = 0, a2 = 0, b0 = 1, b1 = 0, b2 = 0;
        const double cs = std::cos(w0);
        switch (m_kind) {
        case Kind::Equalizer:
            a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
            b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A;
            break;
        case Kind::Bass:
            beta = std::sqrt((A * A + 1) - (A - 1) * (A - 1));
            a0 = (A + 1) + (A - 1) * cs + beta * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cs);
            a2 = (A + 1) + (A - 1) * cs - beta * alpha;
            b0 = A * ((A + 1) - (A - 1) * cs + beta * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cs);
            b2 = A * ((A + 1) - (A - 1) * cs - beta * alpha);
            break;
        case Kind::Treble:
            beta = std::sqrt((A * A + 1) - (A - 1) * (A - 1));
            a0 = (A + 1) - (A - 1) * cs + beta * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cs);
            a2 = (A + 1) - (A - 1) * cs - beta * alpha;
            b0 = A * ((A + 1) + (A - 1) * cs + beta * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 = A * ((A + 1) + (A - 1) * cs - beta * alpha);
            break;
        case Kind::Lowpass:
            a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
            b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
            break;
        case Kind::Highpass:
            a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
            b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
            break;
        }
        m_a1 = a1 / a0; m_a2 = a2 / a0; m_b0 = b0 / a0; m_b1 = b1 / a0; m_b2 = b2 / a0;
    }
    Kind m_kind;
    double m_frequency, m_q, m_gain;
    double m_a1 = 0, m_a2 = 0, m_b0 = 1, m_b1 = 0, m_b2 = 0;
    bool m_bypass = false;
    int m_rate = 0;
    std::vector<double> m_memory;
};

// --------------------------------------------------------------------- gate

// After FFmpeg's af_agate: a downward expander on the channels' average
// level (squared: RMS detection), a soft knee, a range floor.
class Gate : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Gate>(*this); }
    Gate(double threshold, double ratio, double attack, double release, double range, double knee)
        : m_threshold(threshold), m_ratio(ratio), m_attack(attack), m_release(release), m_range(range), m_knee(knee) {}
    void reset(int, int rate) override {
        m_rate = rate;
        double linThreshold = m_threshold * m_threshold;  // RMS detection
        const double kneeRoot = std::sqrt(m_knee);
        m_attackCoeff = std::min(1., 1. / (m_attack * rate / 4000.));
        m_releaseCoeff = std::min(1., 1. / (m_release * rate / 4000.));
        m_linKneeStop = linThreshold * kneeRoot;
        m_linKneeStart = linThreshold / kneeRoot;
        m_thres = std::log(linThreshold);
        m_kneeStart = std::log(m_linKneeStart);
        m_kneeStop = std::log(m_linKneeStop);
        m_slope = 0;
    }
    void process(float *data, int n, int channels, int rate) override {
        if (rate != m_rate) reset(channels, rate);
        for (int i = 0; i < n; ++i) {
            float *s = data + size_t(i) * channels;
            double level = 0;
            for (int c = 0; c < channels; ++c) level += std::fabs(double(s[c]));
            level /= channels;
            level *= level;
            m_slope += (level - m_slope) * (level > m_slope ? m_attackCoeff : m_releaseCoeff);
            double g = 1.0;
            if (m_slope > 0.0 && m_slope < m_linKneeStop) g = outputGain(m_slope);
            for (int c = 0; c < channels; ++c) s[c] = float(double(s[c]) * g);
        }
    }

private:
    static double hermite(double x, double x0, double x1, double p0, double p1, double m0, double m1) {
        const double width = x1 - x0, t = (x - x0) / width;
        m0 *= width;
        m1 *= width;
        const double t2 = t * t, t3 = t2 * t;
        const double ct0 = p0, ct1 = m0, ct2 = -3 * p0 - 2 * m0 + 3 * p1 - m1, ct3 = 2 * p0 + m0 - 2 * p1 + m1;
        return ct3 * t3 + ct2 * t2 + ct1 * t + ct0;
    }
    double outputGain(double linSlope) const {
        const double slope = std::log(linSlope);
        // Downward expansion below the threshold; the knee eases into it.
        double gain = (slope - m_thres) * m_ratio + m_thres;
        if (m_knee > 1. && slope > m_kneeStart)
            gain = hermite(slope, m_kneeStart, m_kneeStop, (m_kneeStart - m_thres) * m_ratio + m_thres, m_kneeStop, m_ratio, 1.);
        return std::max(m_range, std::exp(gain - slope));
    }
    double m_threshold, m_ratio, m_attack, m_release, m_range, m_knee;
    double m_attackCoeff = 1, m_releaseCoeff = 1, m_linKneeStart = 0, m_linKneeStop = 0, m_thres = 0, m_kneeStart = 0,
           m_kneeStop = 0, m_slope = 0;
    int m_rate = 0;
};

// ------------------------------------------------------------------ limiter

// After FFmpeg's af_alimiter (no auto release, no auto level, no latency
// compensation): a look-ahead of the attack time, the gain gliding down to
// the coming peak and back up over the release; the output is that
// look-ahead later, as it always was.
class Limiter : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Limiter>(*this); }
    Limiter(double limit, double attackMs, double releaseMs) : m_limit(limit), m_attack(attackMs), m_release(releaseMs) {}
    void reset(int channels, int rate) override {
        m_channels = channels;
        m_rate = rate;
        const size_t capacity = size_t(rate) * channels / 10 + channels;
        m_buffer.assign(capacity, 0.);
        m_nextDelta.assign(capacity, 0.);
        m_nextPos.assign(capacity, -1);
        m_size = int(rate * (m_attack / 1000.) * channels);
        m_size -= m_size % channels;
        if (m_size <= 0) m_size = channels;
        m_pos = 0;
        m_att = 1.;
        m_delta = 0.;
        m_nextIter = 0;
        m_nextLen = 0;
    }
    void process(float *data, int n, int channels, int rate) override {
        if (channels != m_channels || rate != m_rate) reset(channels, rate);
        const double release = m_release / 1000.;
        const double limit = m_limit;
        for (int i = 0; i < n; ++i) {
            float *s = data + size_t(i) * channels;
            double peak = 0;
            for (int c = 0; c < channels; ++c) {
                const double sample = s[c];
                m_buffer[size_t(m_pos + c)] = sample;
                peak = std::max(peak, std::fabs(sample));
            }
            if (peak > limit) {
                const double patt = std::min(limit / peak, 1.);
                const double rdelta = (1.0 - patt) / (rate * release);
                const double delta = (limit / peak - m_att) / m_size * channels;
                bool found = false;
                if (delta < m_delta) {
                    m_delta = delta;
                    m_nextPos[0] = m_pos;
                    m_nextPos[1] = -1;
                    m_nextDelta[0] = rdelta;
                    m_nextLen = 1;
                    m_nextIter = 0;
                } else {
                    int k = m_nextIter;
                    for (; k < m_nextIter + m_nextLen; ++k) {
                        const int j = k % m_size;
                        double ppeak = 0;
                        if (m_nextPos[size_t(j)] >= 0)
                            for (int c = 0; c < channels; ++c) ppeak = std::max(ppeak, std::fabs(m_buffer[size_t(m_nextPos[size_t(j)] + c)]));
                        const int ahead = ((m_size - m_nextPos[size_t(j)] + m_pos) % m_size) / channels; // whole frames
                        const double pdelta = (limit / peak - limit / ppeak) / ahead;
                        if (pdelta < m_nextDelta[size_t(j)]) {
                            m_nextDelta[size_t(j)] = pdelta;
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        m_nextLen = k - m_nextIter + 1;
                        m_nextPos[size_t((m_nextIter + m_nextLen) % m_size)] = m_pos;
                        m_nextDelta[size_t((m_nextIter + m_nextLen) % m_size)] = rdelta;
                        m_nextPos[size_t((m_nextIter + m_nextLen + 1) % m_size)] = -1;
                        m_nextLen++;
                    }
                }
            }
            const double *buf = &m_buffer[size_t((m_pos + channels) % m_size)];
            peak = 0;
            for (int c = 0; c < channels; ++c) peak = std::max(peak, std::fabs(buf[c]));
            m_att += m_delta;
            double out[8];
            for (int c = 0; c < channels && c < 8; ++c) out[c] = buf[c] * m_att;
            if ((m_pos + channels) % m_size == m_nextPos[size_t(m_nextIter)]) {
                m_delta = m_nextDelta[size_t(m_nextIter)];
                m_att = limit / peak;
                m_nextLen -= 1;
                m_nextPos[size_t(m_nextIter)] = -1;
                m_nextIter = (m_nextIter + 1) % m_size;
            }
            if (m_att > 1.) {
                m_att = 1.;
                m_delta = 0.;
                m_nextIter = 0;
                m_nextLen = 0;
                m_nextPos[0] = -1;
            }
            if (m_att <= 0.) {
                m_att = 0.0000000000001;
                m_delta = (1.0 - m_att) / (rate * release);
            }
            if (m_att != 1. && (1. - m_att) < 0.0000000000001) m_att = 1.;
            if (m_delta != 0. && std::fabs(m_delta) < 0.00000000000001) m_delta = 0.;
            for (int c = 0; c < channels && c < 8; ++c) s[c] = float(std::clamp(out[c], -limit, limit));
            m_pos = (m_pos + channels) % m_size;
        }
    }

private:
    double m_limit, m_attack, m_release;
    int m_channels = 0, m_rate = 0, m_size = 0, m_pos = 0, m_nextIter = 0, m_nextLen = 0;
    double m_att = 1, m_delta = 0;
    std::vector<double> m_buffer, m_nextDelta;
    std::vector<int> m_nextPos;
};

// --------------------------------------------------------------- compressor

// The editor's compressor: peak detection, a soft knee, attack and release
// on the gain reduction (in dB), makeup gain.
class Compressor : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Compressor>(*this); }
    explicit Compressor(const QVariantMap &p) : m_p(p) {}
    void reset(int, int) override { m_reduction = 0; }
    void process(float *data, int n, int ch, int rate) override {
        const auto &p = m_p;
        const double threshold = std::clamp(number(p, "threshold", -18), -80., 0.);
        const double ratio = std::clamp(number(p, "ratio", 4), 1., 20.);
        const double knee = std::clamp(number(p, "knee", 6), 0., 24.);
        const double attack = std::exp(-1. / (rate * std::clamp(number(p, "attack", 10), .01, 2000.) / 1000.));
        const double release = std::exp(-1. / (rate * std::clamp(number(p, "release", 100), .01, 9000.) / 1000.));
        const double makeup = gain(number(p, "makeup"));
        double maximum = 0;
        reductions.assign(static_cast<size_t>(n), 0.);
        for (int i = 0; i < n; ++i) {
            double peak = 0;
            for (int c = 0; c < ch; ++c) peak = std::max(peak, std::abs(double(data[i * ch + c])));
            const double over = 20 * std::log10(std::max(1e-10, peak)) - threshold;
            double wanted = 0;
            if (knee > 0 && over > -knee / 2 && over < knee / 2)
                wanted = (1 - 1 / ratio) * std::pow(over + knee / 2, 2) / (2 * knee);
            else if (over >= knee / 2)
                wanted = (1 - 1 / ratio) * over;
            const double smoothing = wanted > m_reduction ? attack : release;
            m_reduction = smoothing * m_reduction + (1 - smoothing) * wanted;
            maximum = std::max(maximum, m_reduction);
            reductions[size_t(i)] = m_reduction;
            for (int c = 0; c < ch; ++c) data[i * ch + c] *= makeup * gain(-m_reduction);
        }
        reduction = maximum;
    }

private:
    QVariantMap m_p;
    double m_reduction = 0;
};

// ----------------------------------------------------------------- de-esser

// The editor's de-esser: a high-pass at the tuning frequency listens for
// sibilance; while that band's share of the energy stays above a threshold
// set by the intensity, a high shelf at the same frequency turns it down.
class Deesser : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Deesser>(*this); }
    explicit Deesser(const QVariantMap &p) : m_p(p) {}
    void reset(int channels, int rate) override {
        m_detect.assign(size_t(channels), {});
        m_shelf.assign(size_t(channels), {});
        m_high = m_full = m_cut = 0;
        m_rate = rate;
        m_settle = rate / 200;  // a cold filter rings: 5 ms before judging
    }
    void process(float *out, int n, int ch, int rate) override {
        const auto &p = m_p;
        const double maxCut = 48 * (1 - std::clamp(number(p, "max", .5), 0., 1.));
        if (maxCut <= 0) return;
        if (int(m_detect.size()) != ch || m_rate != rate) reset(ch, rate);
        const double frequency = std::clamp(number(p, "frequency", 6500), 1000., std::min(16000., rate * .45));
        const double threshold = -6 - 24 * std::clamp(number(p, "intensity", .5), 0., 1.);
        const double w = 2 * M_PI * frequency / rate, sine = std::sin(w), c = std::cos(w);
        const double a0 = 1 + sine * M_SQRT1_2;
        const double b0 = (1 + c) / 2 / a0, b1 = -(1 + c) / a0, a1 = -2 * c / a0, a2 = (1 - sine * M_SQRT1_2) / a0;
        double shelf[5] = {1, 0, 0, 0, 0};
        const auto design = [&](double db) {
            const double A = std::pow(10., -db / 40), root = 2 * std::sqrt(A) * sine * M_SQRT1_2;
            const double d0 = (A + 1) - (A - 1) * c + root;
            shelf[0] = A * ((A + 1) + (A - 1) * c + root) / d0;
            shelf[1] = -2 * A * ((A - 1) + (A + 1) * c) / d0;
            shelf[2] = A * ((A + 1) + (A - 1) * c - root) / d0;
            shelf[3] = 2 * ((A - 1) - (A + 1) * c) / d0;
            shelf[4] = ((A + 1) - (A - 1) * c - root) / d0;
        };
        const double attack = 1 - std::exp(-1. / (rate * .001)), release = 1 - std::exp(-1. / (rate * .06));
        double designed = -1;
        for (int i = 0; i < n; ++i) {
            double highPower = 0, fullPower = 0;
            for (int chn = 0; chn < ch; ++chn) {
                auto &d = m_detect[size_t(chn)];
                const double x = out[i * ch + chn];
                const double y = b0 * x + b1 * d.x1 + b0 * d.x2 - a1 * d.y1 - a2 * d.y2;
                d.x2 = d.x1; d.x1 = x; d.y2 = d.y1; d.y1 = y;
                highPower = std::max(highPower, y * y);
                fullPower = std::max(fullPower, x * x);
            }
            m_high += (highPower - m_high) * (highPower > m_high ? attack : release);
            m_full += (fullPower - m_full) * (fullPower > m_full ? attack : release);
            double wanted = 0;
            if (m_settle > 0) --m_settle;
            else if (m_full >= 1e-8)
                wanted = std::clamp(2 * (10 * std::log10(std::max(m_high, 1e-20) / m_full) - threshold), 0., maxCut);
            m_cut += (wanted - m_cut) * (wanted > m_cut ? attack : release);
            if (m_cut < .01) {
                m_cut = 0;
                for (int chn = 0; chn < ch; ++chn) {
                    auto &h = m_shelf[size_t(chn)];
                    h.x2 = h.x1; h.y2 = h.y1; h.x1 = h.y1 = out[i * ch + chn];
                }
                continue;
            }
            if (designed < 0 || i % 8 == 0) { design(m_cut); designed = m_cut; }
            for (int chn = 0; chn < ch; ++chn) {
                auto &h = m_shelf[size_t(chn)];
                const double x = out[i * ch + chn];
                const double y = shelf[0] * x + shelf[1] * h.x1 + shelf[2] * h.x2 - shelf[3] * h.y1 - shelf[4] * h.y2;
                h.x2 = h.x1; h.x1 = x; h.y2 = h.y1; h.y1 = y;
                out[i * ch + chn] = float(y);
            }
        }
    }

private:
    struct Filter { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    QVariantMap m_p;
    std::vector<Filter> m_detect, m_shelf;
    double m_high = 0, m_full = 0, m_cut = 0;
    int m_rate = 0, m_settle = 0;
};

// ----------------------------------------------------------------- denoiser

// Burg's method: coefficients d with x[i] ≈ Σ d[k]·x[i-1-k].
std::vector<double> predictor(const double *x, int n, int m) {
    std::vector<double> d(static_cast<size_t>(m), 0.), wk1(static_cast<size_t>(n)), wk2(static_cast<size_t>(n)), wkm(static_cast<size_t>(m));
    wk1[0] = x[0]; wk2[size_t(n - 2)] = x[n - 1];
    for (int j = 1; j < n - 1; ++j) { wk1[size_t(j)] = x[j]; wk2[size_t(j - 1)] = x[j]; }
    for (int k = 0; k < m; ++k) {
        double num = 0, den = 0;
        for (int j = 0; j < n - k - 1; ++j) { num += wk1[size_t(j)] * wk2[size_t(j)]; den += wk1[size_t(j)] * wk1[size_t(j)] + wk2[size_t(j)] * wk2[size_t(j)]; }
        if (den <= 1e-30) break;
        d[size_t(k)] = 2 * num / den;
        for (int i = 0; i < k; ++i) d[size_t(i)] = wkm[size_t(i)] - d[size_t(k)] * wkm[size_t(k - 1 - i)];
        if (k == m - 1) break;
        for (int i = 0; i <= k; ++i) wkm[size_t(i)] = d[size_t(i)];
        for (int j = 0; j < n - k - 2; ++j) { wk1[size_t(j)] -= wkm[size_t(k)] * wk2[size_t(j)]; wk2[size_t(j)] = wk2[size_t(j + 1)] - wkm[size_t(k)] * wk1[size_t(j + 1)]; }
    }
    return d;
}

// The editor's denoiser: windowed spectral noise reduction on one analysis
// grid for the whole run, the future part of a window predicted from the
// past (no playback delay), a decision-directed Wiener gain, a learned
// noise estimate. Processes one timeline frame per call.
class Denoise : public Insert {
public:
    std::unique_ptr<Insert> clone() const override { return std::make_unique<Denoise>(*this); }
    static constexpr int size = 1024, hop = size / 4, order = 32;
    explicit Denoise(const QVariantMap &p) : m_p(p), m_input(size), m_spectrum(size), m_output(size), m_window(size) {
        av_tx_init(&m_forward, &m_fft, AV_TX_FLOAT_FFT, 0, size, nullptr, AV_TX_UNALIGNED);
        av_tx_init(&m_inverse, &m_ifft, AV_TX_FLOAT_FFT, 1, size, nullptr, AV_TX_UNALIGNED);
        for (int i = 0; i < size; ++i) {
            m_window[size_t(i)] = .5 - .5 * std::cos(2 * M_PI * i / size);
            m_windowEnergy += m_window[size_t(i)] * m_window[size_t(i)];
        }
    }
    // A copy carries the state and makes its own transforms.
    Denoise(const Denoise &o)
        : Insert(o), m_p(o.m_p), m_input(o.m_input), m_spectrum(o.m_spectrum), m_output(o.m_output),
          m_window(o.m_window), m_noise(o.m_noise), m_clean(o.m_clean), m_past(o.m_past),
          m_windowEnergy(o.m_windowEnergy), m_at(o.m_at), m_channels(o.m_channels), m_rate(o.m_rate),
          m_fresh(o.m_fresh) {
        av_tx_init(&m_forward, &m_fft, AV_TX_FLOAT_FFT, 0, size, nullptr, AV_TX_UNALIGNED);
        av_tx_init(&m_inverse, &m_ifft, AV_TX_FLOAT_FFT, 1, size, nullptr, AV_TX_UNALIGNED);
    }
    Denoise &operator=(const Denoise &) = delete;
    ~Denoise() override { av_tx_uninit(&m_forward); av_tx_uninit(&m_inverse); }
    void reset(int, int) override { m_fresh = true; }
    void process(float *data, int n, int ch, int rate) override {
        if (!m_fft || !m_ifft || n <= 0) return;
        const auto &p = m_p;
        const double reductionDb = std::clamp(number(p, "reduction", 12), 0., 97.);
        if (reductionDb == 0) return;
        const double minimum = gain(-reductionDb);
        const double baseNoise = std::pow(10., std::clamp(number(p, "floor", -50), -80., -20.) / 10.) * m_windowEnergy;
        const bool track = p.value("track", true).toBool();
        constexpr int bins = size / 2 + 1;
        const float *source = data;
        if (m_fresh || m_channels != ch || m_rate != rate) {
            m_noise.assign(size_t(ch) * bins, baseNoise);
            m_clean.assign(size_t(ch) * bins, 0);
            m_past.assign(size_t(ch) * size, 0);
            for (int i = 0; i < size; ++i) {
                int sample = (i - size) % (2 * n);
                if (sample < 0) sample += 2 * n;
                if (sample >= n) sample = 2 * n - 1 - sample;
                for (int channel = 0; channel < ch; ++channel)
                    m_past[size_t(channel) * size + size_t(i)] = source[sample * ch + channel];
            }
            m_at = 0;
            m_channels = ch;
            m_rate = rate;
            m_fresh = false;
        }
        const int known = size + n, span = known + size, fit = std::min(known, 2 * size);
        std::vector<double> signal(static_cast<size_t>(span)), power(bins), gains(bins), smooth(bins), kept(bins), weight(static_cast<size_t>(n), 0.);
        Samples result(size_t(n) * ch, 0);
        long long first = m_at - size + 1;
        first += ((hop - first % hop) % hop + hop) % hop;
        for (int channel = 0; channel < ch; ++channel) {
            for (int i = 0; i < size; ++i) signal[size_t(i)] = m_past[size_t(channel) * size + size_t(i)];
            for (int i = 0; i < n; ++i) signal[size_t(size + i)] = source[i * ch + channel];
            const auto d = predictor(signal.data() + known - fit, fit, order);
            for (int i = known; i < span; ++i) {
                double v = 0;
                for (int k = 0; k < order; ++k) v += d[size_t(k)] * signal[size_t(i - 1 - k)];
                signal[size_t(i)] = v;
            }
            std::copy(m_clean.begin() + channel * bins, m_clean.begin() + (channel + 1) * bins, kept.begin());
            for (long long start = first; start < m_at + n; start += hop) {
                const int offset = int(start - m_at) + size;
                const bool final = start + size <= m_at + n;
                for (int i = 0; i < size; ++i) m_input[size_t(i)] = {float(signal[size_t(offset + i)] * m_window[size_t(i)]), 0};
                m_fft(m_forward, m_spectrum.data(), m_input.data(), sizeof(AVComplexFloat));
                for (int k = 0; k < bins; ++k) {
                    const auto bin = m_spectrum[size_t(k)];
                    power[size_t(k)] = double(bin.re) * bin.re + double(bin.im) * bin.im;
                }
                for (int k = 0; k < bins; ++k) {
                    const double energy = (power[size_t(std::max(0, k - 1))] + 2 * power[size_t(k)] + power[size_t(std::min(size / 2, k + 1))]) / 4;
                    auto &estimate = m_noise[size_t(channel * bins + k)];
                    if (track && final && start >= 0) {
                        if (energy < 3 * estimate) estimate += (energy - estimate) * .03;
                        else estimate *= 1.01;
                        estimate = std::clamp(estimate, baseNoise * .1, baseNoise * 10);
                    }
                    const double noise = track ? 1.4 * estimate : baseNoise;
                    const double snr = .98 * kept[size_t(k)] / noise + .02 * std::max(energy / noise - 1, 0.);
                    gains[size_t(k)] = std::max(minimum, snr / (1 + snr));
                    kept[size_t(k)] = gains[size_t(k)] * gains[size_t(k)] * energy;
                }
                if (final) std::copy(kept.begin(), kept.end(), m_clean.begin() + channel * bins);
                for (int k = 0; k < bins; ++k)
                    smooth[size_t(k)] = (gains[size_t(std::max(0, k - 1))] + 2 * gains[size_t(k)] + gains[size_t(std::min(size / 2, k + 1))]) / 4;
                for (int k = 0; k < size; ++k) {
                    const double g = smooth[size_t(k <= size / 2 ? k : size - k)];
                    m_spectrum[size_t(k)].re *= float(g);
                    m_spectrum[size_t(k)].im *= float(g);
                }
                m_ifft(m_inverse, m_output.data(), m_spectrum.data(), sizeof(AVComplexFloat));
                for (int i = 0; i < size; ++i) {
                    const int sample = offset - size + i;
                    if (sample < 0 || sample >= n) continue;
                    result[size_t(sample) * ch + size_t(channel)] += float(m_output[size_t(i)].re / size * m_window[size_t(i)]);
                    if (channel == 0) weight[size_t(sample)] += m_window[size_t(i)] * m_window[size_t(i)];
                }
            }
            for (int i = 0; i < size; ++i) m_past[size_t(channel) * size + size_t(i)] = signal[size_t(n + i)];
        }
        m_at += n;
        for (int i = 0; i < n; ++i)
            for (int channel = 0; channel < ch; ++channel)
                data[size_t(i) * ch + size_t(channel)] = float(result[size_t(i) * ch + size_t(channel)] / std::max(weight[size_t(i)], 1e-20));
    }

private:
    QVariantMap m_p;
    AVTXContext *m_forward = nullptr, *m_inverse = nullptr;
    av_tx_fn m_fft = nullptr, m_ifft = nullptr;
    std::vector<AVComplexFloat> m_input, m_spectrum, m_output;
    std::vector<double> m_window, m_noise, m_clean, m_past;
    double m_windowEnergy = 0;
    long long m_at = 0;
    int m_channels = 0, m_rate = 0;
    bool m_fresh = true;
};

// --------------------------------------------------------------- one effect

// One fx entry: its filters in order, wet/dry mixed when its mix is below 1.
class Effect : public Insert {
public:
    std::unique_ptr<Insert> clone() const override {
        std::vector<std::unique_ptr<Insert>> parts;
        parts.reserve(m_parts.size());
        for (const auto &p : m_parts) parts.push_back(p->clone());
        auto copy = std::make_unique<Effect>(std::move(parts), m_wet);
        copy->m_dry = m_dry;
        copy->reduction = reduction;
        copy->reductions = reductions;
        return copy;
    }
    Effect(std::vector<std::unique_ptr<Insert>> parts, double wet) : m_parts(std::move(parts)), m_wet(wet) {}
    void reset(int channels, int rate) override {
        for (auto &p : m_parts) p->reset(channels, rate);
    }
    void process(float *data, int n, int channels, int rate) override {
        const bool blend = m_wet >= 0 && m_wet < 1;
        if (blend) m_dry.assign(data, data + size_t(n) * channels);
        reduction = 0;
        reductions.clear();
        for (auto &p : m_parts) {
            p->process(data, n, channels, rate);
            reduction = std::max(reduction, p->reduction);
            if (!p->reductions.empty()) {
                if (reductions.empty()) reductions.assign(static_cast<size_t>(n), 0.);
                for (int i = 0; i < n && i < int(p->reductions.size()); ++i) reductions[size_t(i)] = std::max(reductions[size_t(i)], p->reductions[size_t(i)]);
            }
        }
        if (blend) {
            const float wet = float(m_wet);
            for (size_t i = 0; i < m_dry.size(); ++i) data[i] = m_dry[i] + wet * (data[i] - m_dry[i]);
        }
    }

private:
    std::vector<std::unique_ptr<Insert>> m_parts;
    double m_wet;
    Samples m_dry;
};

QVariantList catalogue() {
    const auto entry = [](const QString &id, const QVariantMap &params) { return QVariant(QVariantMap{{"id", id}, {"params", params}}); };
    QVariantList bands;
    const double freqs[5] = {100, 350, 1000, 3500, 10000};
    const double qs[5] = {0.7, 1.0, 1.0, 1.0, 0.7};
    for (int i = 0; i < 5; ++i) bands.append(QVariant(QVariantMap{{"f", freqs[i]}, {"g", 0.0}, {"q", qs[i]}}));
    return {
        entry("eq", {{"bands", bands}}),
        entry("comp", {{"threshold", -20.0}, {"ratio", 3.0}, {"attack", 10.0}, {"release", 120.0}, {"knee", 6.0}, {"makeup", 2.0}}),
        entry("gate", {{"threshold", -40.0}, {"ratio", 2.0}, {"attack", 20.0}, {"release", 250.0}, {"range", -60.0}, {"knee", 6.0}}),
        entry("limiter", {{"ceiling", -1.0}, {"attack", 5.0}, {"release", 50.0}}),
        entry("deesser", {{"frequency", 6500.0}, {"intensity", 0.5}, {"max", 0.5}}),
        entry("nr", {{"reduction", 12.0}, {"floor", -50.0}, {"track", true}}),
        entry("highpass", {{"frequency", 80.0}}),
        entry("hum", {{"frequency", 50.0}, {"reduction", 36.0}}),
        entry("lowpass", {{"frequency", 12000.0}}),
    };
}

}  // namespace

QVariantMap withDefaults(const QString &id, const QVariantMap &params) {
    QVariantMap out = params;
    for (const QVariant &v : catalogue()) {
        const QVariantMap e = v.toMap();
        if (e.value("id").toString() != id) continue;
        const QVariantMap defaults = e.value("params").toMap();
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it)
            if (!out.contains(it.key())) out.insert(it.key(), it.value());
    }
    return out;
}

std::vector<std::unique_ptr<Insert>> insertsFor(const QVariantList &fx) {
    std::vector<std::unique_ptr<Insert>> out;
    for (const QVariant &v : fx) {
        const QVariantMap e = v.toMap();
        const QString id = e.value("id").toString();
        if (e.value("bypass").toBool()) continue;
        const double wet = e.contains("mix") ? std::clamp(e.value("mix").toDouble(), 0., 1.) : -1.;
        const QVariantMap p = withDefaults(id, e.value("params").toMap());
        std::vector<std::unique_ptr<Insert>> parts;
        if (id == "eq") {
            const QVariantList bands = p.value("bands").toList();
            for (int b = 0; b < bands.size(); ++b) {
                const QVariantMap band = bands.at(b).toMap();
                const double g = band.value("g").toDouble();
                if (std::abs(g) < 0.05) continue;
                const double freq = std::max(20.0, band.value("f").toDouble());
                const double q = std::max(0.1, band.value("q", 1.0).toDouble());
                const auto kind = b == 0 ? Biquad::Kind::Bass : b == bands.size() - 1 ? Biquad::Kind::Treble : Biquad::Kind::Equalizer;
                parts.push_back(std::make_unique<Biquad>(kind, freq, q, std::clamp(g, -24.0, 24.0)));
            }
        } else if (id == "comp") {
            parts.push_back(std::make_unique<Compressor>(p));
        } else if (id == "gate") {
            parts.push_back(std::make_unique<Gate>(std::max(0.0, dbToLinear(p.value("threshold").toDouble())),
                                                   std::clamp(p.value("ratio").toDouble(), 1.0, 9000.0),
                                                   std::clamp(p.value("attack").toDouble(), 0.01, 9000.0),
                                                   std::clamp(p.value("release").toDouble(), 0.01, 9000.0),
                                                   std::clamp(dbToLinear(p.value("range").toDouble()), 0.0, 1.0),
                                                   std::clamp(dbToLinear(p.value("knee").toDouble()), 1.0, 8.0)));
        } else if (id == "limiter") {
            parts.push_back(std::make_unique<Limiter>(std::clamp(dbToLinear(p.value("ceiling").toDouble()), 0.0625, 1.0),
                                                      std::clamp(p.value("attack").toDouble(), 0.1, 80.0),
                                                      std::clamp(p.value("release").toDouble(), 1.0, 8000.0)));
        } else if (id == "deesser") {
            parts.push_back(std::make_unique<Deesser>(p));
        } else if (id == "hum") {
            const double hz = std::clamp(p.value("frequency", 50).toDouble(), 40., 120.);
            for (int harmonic = 1; harmonic <= 4; ++harmonic)
                parts.push_back(std::make_unique<Biquad>(Biquad::Kind::Equalizer, hz * harmonic, 20.0,
                                                         -std::clamp(p.value("reduction", 36).toDouble(), 0., 60.)));
        } else if (id == "nr") {
            parts.push_back(std::make_unique<Denoise>(p));
        } else if (id == "highpass" || id == "lowpass") {
            parts.push_back(std::make_unique<Biquad>(id == "highpass" ? Biquad::Kind::Highpass : Biquad::Kind::Lowpass,
                                                     std::clamp(p.value("frequency").toDouble(), 10.0, 22000.0), 0.707, 0));
        } else {
            continue;
        }
        out.push_back(std::make_unique<Effect>(std::move(parts), wet));
    }
    return out;
}

std::vector<std::unique_ptr<Insert>> presetInserts(const QString &preset) {
    std::vector<std::unique_ptr<Insert>> out;
    // The defaults those filters have always had: bass 100 Hz, treble 3 kHz,
    // shelves at Q 0.5; the equalizer's width is a Q.
    if (preset == "bass-boost") out.push_back(std::make_unique<Biquad>(Biquad::Kind::Bass, 100, 0.5, 6));
    else if (preset == "bass-reduce") out.push_back(std::make_unique<Biquad>(Biquad::Kind::Bass, 100, 0.5, -6));
    else if (preset == "treble-boost") out.push_back(std::make_unique<Biquad>(Biquad::Kind::Treble, 3000, 0.5, 6));
    else if (preset == "treble-reduce") out.push_back(std::make_unique<Biquad>(Biquad::Kind::Treble, 3000, 0.5, -6));
    else if (preset == "voice") {
        out.push_back(std::make_unique<Biquad>(Biquad::Kind::Equalizer, 2500, 1000, 4));
        out.push_back(std::make_unique<Biquad>(Biquad::Kind::Bass, 100, 0.5, -2));
    } else if (preset == "hum") {
        out.push_back(std::make_unique<Biquad>(Biquad::Kind::Equalizer, 50, 20, -24));
    }
    return out;
}

}  // namespace sound
