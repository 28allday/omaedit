#include "beatmap.h"

extern "C" {
#include <libavutil/tx.h>
}

#include <algorithm>
#include <cmath>
#include <thread>

namespace BeatMap {
namespace {

constexpr int kBands = 24;
// The onset strength compares a frame with the one this many before it: a
// drum's attack is over in less, a bowed note's is not.
constexpr int kFluxLag = 2;
// Frames made at a time: about twelve seconds, shared among the threads.
constexpr size_t kBlock = 2048;
// The tempi looked for. Outside them a pulse is a bar or a subdivision.
constexpr double kMinBpm = 40, kMaxBpm = 240;
// How strongly the beats keep to the period (Ellis's "tightness"): a beat a
// tenth of a period early or late costs about one strong onset.
constexpr double kTightness = 100;
// Below this the onsets do not repeat at any period: speech, noise, a drone.
constexpr double kMinStrength = 0.12;
// And the onsets under the beats found must stand this far above the rest
// (the onset strength has unit deviation).
constexpr double kMinSalience = 0.6;
// An attack is heard where it starts. The onset strength is at its highest
// while the attack is still coming into the frame, this far along it (the
// logarithmic scale makes much of the little that is in so far): measured on
// clicks and drums in tst_beatmap, where it leaves a mean error near nothing.
constexpr double kAttackAt = 0.655;
// The bar. Below this a band is a bass drum's or a bass note's.
constexpr double kLowHz = 200;
// The bands are kept, this many frames to a slice, to hear the chords change.
constexpr int kSlice = 4;
// The first beat of the bar must stand above the mean of the four by this
// many times what chance gives a quarter of the beats (their accents'
// deviation over the root of their number), or the bars count from the
// first beat.
constexpr double kMinBarLean = 2.5;

int frameSize(int rate) {
    int size = 256;
    while (size < rate * 0.04) size *= 2; // 512 at 11025 Hz, 2048 at 44.1 and 48 kHz
    return size;
}

// The peak of a parabola through three points a step apart: its place from
// the middle one (-0.5 to 0.5) and its height.
std::pair<double, double> parabola(double a, double b, double c) {
    const double bend = a - 2 * b + c;
    if (bend >= 0) return {0., b};
    const double at = std::clamp(0.5 * (a - c) / bend, -0.5, 0.5);
    return {at, b - 0.25 * (a - c) * at};
}

}

struct Analysis::State {
    int rate = kRate, size = 512, hop = 64, threads = 1;
    std::vector<float> window;
    std::vector<int> bandOf;     // spectrum bin -> band, -1 for those left out
    std::vector<float> pending;  // samples from the next frame's first on
    std::vector<float> tail;     // the bands of the last kFluxLag frames made
    std::vector<float> flux;     // onset strength, one a frame
    std::vector<float> low;      // … of the bands under kLowHz alone
    std::vector<float> slices;   // the bands, kSlice frames to each
    float sliceSum[kBands] = {};
    int sliceHeld = 0, lowBands = 0;
    float peak = 0;

    void make(size_t count);
};

Analysis::Analysis(int rate, int threads) : d(std::make_unique<State>()) {
    d->rate = std::clamp(rate, 8000, 48000);
    d->size = frameSize(d->rate);
    d->hop = d->size / 8;
    const int machine = int(std::thread::hardware_concurrency());
    d->threads = std::clamp(threads > 0 ? threads : machine, 1, 8);
    d->window.resize(size_t(d->size));
    for (int i = 0; i < d->size; ++i)
        d->window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / d->size));
    // Bands a third of an octave or so wide from 30 Hz to just under half the rate.
    const int bins = d->size / 2;
    const double low = 30, high = d->rate * 0.475;
    d->bandOf.assign(size_t(bins), -1);
    for (int bin = 1; bin < bins; ++bin) {
        const double hz = double(bin) * d->rate / d->size;
        if (hz < low || hz > high) continue;
        d->bandOf[size_t(bin)] = std::min(kBands - 1, int(std::log(hz / low) / std::log(high / low) * kBands));
    }
    d->lowBands = std::clamp(int(std::ceil(std::log(kLowHz / low) / std::log(high / low) * kBands)), 1, kBands);
}

Analysis::~Analysis() = default;

// The next `count` frames from the front of `pending`: their bands, each on a
// logarithmic scale, then how much each frame rose over the one kFluxLag back.
void Analysis::State::make(size_t count) {
    if (count == 0) return;
    std::vector<float> bands(count * kBands);
    const auto work = [&](size_t from, size_t to) {
        AVTXContext *context = nullptr;
        av_tx_fn transform = nullptr;
        if (av_tx_init(&context, &transform, AV_TX_FLOAT_FFT, 0, size, nullptr, AV_TX_UNALIGNED) < 0 || !transform) return;
        std::vector<AVComplexFloat> in(static_cast<size_t>(size)), out(static_cast<size_t>(size));
        // A full-scale tone reads 1 in its bin under this window.
        const float scale = 4.f / float(size);
        for (size_t frame = from; frame < to; ++frame) {
            const float *samples = pending.data() + frame * size_t(hop);
            for (int i = 0; i < size; ++i) in[size_t(i)] = {samples[i] * window[size_t(i)], 0.f};
            transform(context, out.data(), in.data(), sizeof(AVComplexFloat));
            float sums[kBands] = {};
            for (int bin = 1; bin < size / 2; ++bin) {
                const int band = bandOf[size_t(bin)];
                if (band >= 0) sums[band] += std::hypot(out[size_t(bin)].re, out[size_t(bin)].im);
            }
            for (int band = 0; band < kBands; ++band)
                bands[frame * kBands + size_t(band)] = std::log1p(100.f * scale * sums[band]);
        }
        av_tx_uninit(&context);
    };
    const size_t workers = std::min<size_t>(size_t(threads), std::max<size_t>(1, count / 64));
    if (workers <= 1) {
        work(0, count);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (size_t w = 0; w < workers; ++w)
            pool.emplace_back(work, count * w / workers, count * (w + 1) / workers);
        for (auto &thread : pool) thread.join();
    }
    // The frames kFluxLag before the first of these are the last ones made.
    const auto earlier = [&](size_t frame, int band) -> float {
        if (frame >= size_t(kFluxLag)) return bands[(frame - kFluxLag) * kBands + size_t(band)];
        const size_t held = tail.size() / kBands, back = size_t(kFluxLag) - frame;
        return back <= held ? tail[(held - back) * kBands + size_t(band)] : bands[frame * kBands + size_t(band)];
    };
    for (size_t frame = 0; frame < count; ++frame) {
        float rise = 0, deep = 0;
        for (int band = 0; band < kBands; ++band) {
            const float up = std::max(0.f, bands[frame * kBands + size_t(band)] - earlier(frame, band));
            rise += up;
            if (band < lowBands) deep += up;
            sliceSum[band] += bands[frame * kBands + size_t(band)];
        }
        flux.push_back(rise);
        low.push_back(deep);
        if (++sliceHeld == kSlice) {
            for (float &sum : sliceSum) { slices.push_back(sum / kSlice); sum = 0; }
            sliceHeld = 0;
        }
    }
    std::vector<float> kept;
    const size_t keep = std::min<size_t>(size_t(kFluxLag), count);
    if (keep < size_t(kFluxLag) && !tail.empty()) {
        const size_t old = std::min(tail.size() / kBands, size_t(kFluxLag) - keep);
        kept.assign(tail.end() - std::ptrdiff_t(old * kBands), tail.end());
    }
    kept.insert(kept.end(), bands.end() - std::ptrdiff_t(keep * kBands), bands.end());
    tail = std::move(kept);
    pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(count * size_t(hop)));
}

void Analysis::push(const float *mono, size_t count) {
    if (!mono || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        const float value = std::isfinite(mono[i]) ? mono[i] : 0.f;
        d->peak = std::max(d->peak, std::abs(value));
        d->pending.push_back(value);
    }
    // Whole blocks only, so the frames are the same however the sound arrives.
    const size_t need = size_t(d->size) + (kBlock - 1) * size_t(d->hop);
    while (d->pending.size() >= need) d->make(kBlock);
}

Result Analysis::finish(const std::function<bool()> &cancelled) {
    Result result;
    const auto stop = [&] { return cancelled && cancelled(); };
    if (d->pending.size() >= size_t(d->size))
        d->make((d->pending.size() - size_t(d->size)) / size_t(d->hop) + 1);
    const double fps = double(d->rate) / d->hop;
    const auto count = std::ptrdiff_t(d->flux.size());
    // Under eight seconds a few syllables or footsteps pass for a tempo; -80 dB is silence.
    if (count < std::ptrdiff_t(fps * 8) || d->peak < 1e-4f) return result;
    if (stop()) { result.cancelled = true; return result; }

    // The onset strength: what stands above its own surroundings (a second
    // and a half of them), lightly smoothed, in units of its deviation.
    std::vector<double> onset(static_cast<size_t>(count));
    {
        const auto half = std::ptrdiff_t(fps * 0.75);
        std::vector<double> sums(static_cast<size_t>(count) + 1, 0.);
        for (std::ptrdiff_t i = 0; i < count; ++i) sums[size_t(i) + 1] = sums[size_t(i)] + double(d->flux[size_t(i)]);
        std::vector<double> raised(static_cast<size_t>(count));
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            const std::ptrdiff_t from = std::max<std::ptrdiff_t>(0, i - half), to = std::min(count, i + half + 1);
            raised[size_t(i)] = double(d->flux[size_t(i)]) - (sums[size_t(to)] - sums[size_t(from)]) / double(to - from);
        }
        double power = 0;
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            const double before = raised[size_t(std::max<std::ptrdiff_t>(0, i - 1))], after = raised[size_t(std::min(count - 1, i + 1))];
            onset[size_t(i)] = 0.25 * before + 0.5 * raised[size_t(i)] + 0.25 * after;
            power += onset[size_t(i)] * onset[size_t(i)];
        }
        const double deviation = std::sqrt(power / double(count));
        if (!(deviation > 1e-9)) return result;
        for (double &value : onset) value /= deviation;
    }

    // Tempo. The autocorrelation, each lag over the terms it has, so a long
    // lag is not marked down for having fewer.
    const auto lagMin = std::ptrdiff_t(std::floor(fps * 60 / kMaxBpm)), lagMax = std::ptrdiff_t(std::ceil(fps * 60 / kMinBpm));
    const auto lags = std::min<std::ptrdiff_t>(count / 2, 4 * lagMax + 8);
    if (lags <= lagMin + 2) return result;
    std::vector<double> repeat(static_cast<size_t>(lags) + 1, 0.);
    {
        const size_t workers = count * lags > 4'000'000 ? size_t(d->threads) : 1;
        const auto work = [&](std::ptrdiff_t from, std::ptrdiff_t to) {
            for (std::ptrdiff_t lag = from; lag < to && !stop(); ++lag) {
                double sum = 0;
                const double *a = onset.data(), *b = onset.data() + lag;
                for (std::ptrdiff_t i = 0; i < count - lag; ++i) sum += a[i] * b[i];
                repeat[size_t(lag)] = sum / double(count - lag);
            }
        };
        if (workers <= 1) {
            work(1, lags + 1);
        } else {
            std::vector<std::thread> pool;
            pool.reserve(workers);
            for (size_t w = 0; w < workers; ++w)
                pool.emplace_back(work, 1 + lags * std::ptrdiff_t(w) / std::ptrdiff_t(workers), 1 + lags * std::ptrdiff_t(w + 1) / std::ptrdiff_t(workers));
            for (auto &thread : pool) thread.join();
        }
    }
    if (stop()) { result.cancelled = true; return result; }
    // Its peaks are the periods the onsets repeat at: the beat, and with it
    // the bar, the half bar and the subdivisions. Each is weighed by how
    // readily people tap that tempo (most readily near 120 a minute), and of
    // those within a fifth of the best the quickest is the beat: a pulse
    // heard at a period is heard at every multiple of it too.
    struct Peak { double lag, height, weighed; };
    std::vector<Peak> peaks;
    double best = 0;
    for (std::ptrdiff_t lag = std::max<std::ptrdiff_t>(2, lagMin); lag <= std::min(lagMax, lags - 1); ++lag) {
        const double here = repeat[size_t(lag)];
        if (here <= 0 || here <= repeat[size_t(lag) - 1] || here < repeat[size_t(lag) + 1]) continue;
        const auto [at, height] = parabola(repeat[size_t(lag) - 1], here, repeat[size_t(lag) + 1]);
        const double bpm = 60 * fps / (double(lag) + at);
        const double octaves = std::log2(bpm / 120) / 1.2;
        peaks.push_back({double(lag) + at, height, height * std::exp(-0.5 * octaves * octaves)});
        best = std::max(best, peaks.back().weighed);
    }
    const Peak *beat = nullptr;
    for (const Peak &peak : peaks)
        if (peak.weighed >= 0.8 * best) { beat = &peak; break; }
    if (!beat) return result;
    result.strength = std::clamp(beat->height, 0., 1.);
    if (result.strength < kMinStrength) return result;
    // The period more finely, from where it comes round the second, third
    // and fourth time.
    double period = beat->lag;
    for (int times = 2; times <= 4; ++times) {
        const double centre = period * times;
        const auto reach = std::ptrdiff_t(2 + centre * 0.02);
        std::ptrdiff_t top = -1;
        for (auto lag = std::max<std::ptrdiff_t>(2, std::ptrdiff_t(std::lround(centre)) - reach);
             lag <= std::min(lags - 1, std::ptrdiff_t(std::lround(centre)) + reach); ++lag)
            if (top < 0 || repeat[size_t(lag)] > repeat[size_t(top)]) top = lag;
        if (top < 2 || top >= lags || repeat[size_t(top)] < 0.5 * beat->height
            || repeat[size_t(top)] <= repeat[size_t(top) - 1] || repeat[size_t(top)] < repeat[size_t(top) + 1])
            break;
        period = (double(top) + parabola(repeat[size_t(top) - 1], repeat[size_t(top)], repeat[size_t(top) + 1]).first) / times;
    }

    // The beats. Each frame's score is its onset plus the best score a beat
    // from half a period to two periods earlier could hand on, less what that
    // step's distance from one period costs; the best chain of them is read
    // back from its end.
    std::vector<double> local(static_cast<size_t>(count));
    {
        // Onsets a little apart count as one (a flam, a strummed chord).
        const double sigma = std::max(0.5, period / 32);
        const auto reach = std::ptrdiff_t(std::ceil(3 * sigma));
        std::vector<double> kernel(static_cast<size_t>(2 * reach + 1));
        double total = 0;
        for (std::ptrdiff_t i = -reach; i <= reach; ++i)
            total += kernel[size_t(i + reach)] = std::exp(-0.5 * double(i * i) / (sigma * sigma));
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            double sum = 0;
            for (std::ptrdiff_t k = -reach; k <= reach; ++k)
                if (i + k >= 0 && i + k < count) sum += onset[size_t(i + k)] * kernel[size_t(k + reach)];
            local[size_t(i)] = sum / total;
        }
    }
    const auto nearest = std::max<std::ptrdiff_t>(1, std::ptrdiff_t(std::lround(period / 2)));
    const auto farthest = std::max(nearest + 1, std::ptrdiff_t(std::lround(period * 2)));
    std::vector<double> step(static_cast<size_t>(farthest - nearest + 1));
    for (std::ptrdiff_t back = nearest; back <= farthest; ++back) {
        const double off = std::log(double(back) / period);
        step[size_t(back - nearest)] = -kTightness * off * off;
    }
    std::vector<double> score(static_cast<size_t>(count));
    std::vector<std::ptrdiff_t> from(static_cast<size_t>(count), -1);
    for (std::ptrdiff_t i = 0; i < count; ++i) {
        if ((i & 0xfff) == 0 && stop()) { result.cancelled = true; return result; }
        double carried = 0;
        std::ptrdiff_t source = -1;
        for (std::ptrdiff_t back = nearest; back <= farthest && back <= i; ++back) {
            const double value = score[size_t(i - back)] + step[size_t(back - nearest)];
            if (value > carried) { carried = value; source = i - back; }
        }
        score[size_t(i)] = local[size_t(i)] + carried;
        from[size_t(i)] = source;
    }
    // The chain ends at the last frame that tops its neighbours with a score
    // worth having: at least half the usual such top.
    std::vector<std::ptrdiff_t> tops;
    for (std::ptrdiff_t i = 1; i + 1 < count; ++i)
        if (score[size_t(i)] > score[size_t(i) - 1] && score[size_t(i)] >= score[size_t(i) + 1]) tops.push_back(i);
    if (tops.empty()) return result;
    std::vector<double> heights;
    heights.reserve(tops.size());
    for (const auto top : tops) heights.push_back(score[size_t(top)]);
    std::nth_element(heights.begin(), heights.begin() + std::ptrdiff_t(heights.size() / 2), heights.end());
    const double usual = heights[heights.size() / 2];
    std::ptrdiff_t last = -1;
    for (auto it = tops.rbegin(); it != tops.rend(); ++it)
        if (score[size_t(*it)] * 2 > usual) { last = *it; break; }
    if (last < 0) return result;
    std::vector<std::ptrdiff_t> frames;
    for (std::ptrdiff_t i = last; i >= 0; i = from[size_t(i)]) frames.push_back(i);
    std::reverse(frames.begin(), frames.end());
    // Beats placed across a quiet lead-in or a fade, where nothing was
    // played to hold them, are not the music's: those at either end with
    // weak onsets under them go.
    {
        const auto beats = std::ptrdiff_t(frames.size());
        std::vector<double> under(static_cast<size_t>(beats));
        const double shape[5] = {0., 0.5, 1., 0.5, 0.};
        double power = 0;
        for (std::ptrdiff_t i = 0; i < beats; ++i) {
            double sum = 0;
            for (int k = -2; k <= 2; ++k)
                if (i + k >= 0 && i + k < beats) sum += local[size_t(frames[size_t(i + k)])] * shape[k + 2];
            under[size_t(i)] = sum;
            power += sum * sum;
        }
        const double floor = 0.5 * std::sqrt(power / double(std::max<std::ptrdiff_t>(1, beats)));
        std::ptrdiff_t first = 0, end = beats;
        while (first < end && under[size_t(first)] <= floor) ++first;
        while (end > first && under[size_t(end) - 1] <= floor) --end;
        frames.assign(frames.begin() + first, frames.begin() + end);
    }
    if (frames.size() < 8) return result;
    double salience = 0;
    for (const auto frame : frames) salience += local[size_t(frame)];
    salience /= double(frames.size());
    if (salience < kMinSalience) return result;

    // Each beat to a fraction of a frame, at the time its attack starts.
    result.beats.reserve(frames.size());
    for (const auto frame : frames) {
        double fine = 0;
        if (frame > 0 && frame + 1 < count && local[size_t(frame)] >= local[size_t(frame) - 1] && local[size_t(frame)] >= local[size_t(frame) + 1])
            fine = parabola(local[size_t(frame) - 1], local[size_t(frame)], local[size_t(frame) + 1]).first;
        const double seconds = ((double(frame) + fine) * d->hop + kAttackAt * d->size) / d->rate;
        result.beats.push_back(std::max(0., seconds));
    }
    result.bpm = 60. * double(result.beats.size() - 1) / (result.beats.back() - result.beats.front());

    // The bar: four beats, the first of them the one the music leans on.
    // Each beat's accent is three things, each in its own deviations over
    // the beats: how hard its attack is, how much of it is low (a bass drum,
    // a bass note), and how far the sound of the beat after it is from the
    // sound of the beat before it (a chord changes at the bar). The beats
    // are taken four at a time from each of the four places a bar could
    // start, and the place whose beats have the highest mean accent is it.
    {
        const size_t beats = frames.size();
        const auto reach = std::max<std::ptrdiff_t>(2, std::ptrdiff_t(std::lround(period / 16)));
        const auto sliceCount = std::ptrdiff_t(d->slices.size() / kBands);
        // The bands' mean over the slices from one frame to another.
        const auto sound = [&](std::ptrdiff_t from, std::ptrdiff_t to, double *out) {
            const auto first = std::clamp<std::ptrdiff_t>(from / kSlice, 0, sliceCount), end = std::clamp<std::ptrdiff_t>(to / kSlice, first, sliceCount);
            for (int band = 0; band < kBands; ++band) {
                double sum = 0;
                for (auto slice = first; slice < end; ++slice) sum += double(d->slices[size_t(slice) * kBands + size_t(band)]);
                out[band] = end > first ? sum / double(end - first) : 0.;
            }
            return end > first;
        };
        std::vector<double> parts[3];
        for (auto &part : parts) part.assign(beats, 0.);
        for (size_t i = 0; i < beats; ++i) {
            const auto frame = frames[i];
            parts[0][i] = local[size_t(frame)];
            for (auto k = std::max<std::ptrdiff_t>(0, frame - reach); k <= std::min(count - 1, frame + reach); ++k)
                parts[1][i] = std::max(parts[1][i], double(d->low[size_t(k)]));
            const auto before = i > 0 ? frames[i - 1] : frame - std::ptrdiff_t(std::lround(period));
            const auto after = i + 1 < beats ? frames[i + 1] : frame + std::ptrdiff_t(std::lround(period));
            double was[kBands], is[kBands];
            if (sound(before, frame, was) && sound(frame, after, is))
                for (int band = 0; band < kBands; ++band) parts[2][i] += std::abs(is[band] - was[band]);
        }
        std::vector<double> accent(beats, 0.);
        for (auto &part : parts) {
            double mean = 0, power = 0;
            for (const double value : part) mean += value;
            mean /= double(beats);
            for (const double value : part) power += (value - mean) * (value - mean);
            const double deviation = std::sqrt(power / double(beats));
            // Beats all alike in this (a click track) say nothing about the bar.
            if (!(deviation > 0.05 * std::abs(mean)) || !(deviation > 1e-9)) continue;
            for (size_t i = 0; i < beats; ++i) accent[i] += (part[i] - mean) / deviation;
        }
        double sums[4] = {}, mean = 0, power = 0;
        size_t held[4] = {};
        for (size_t i = 0; i < beats; ++i) { sums[i % 4] += accent[i]; ++held[i % 4]; mean += accent[i]; }
        mean /= double(beats);
        for (const double value : accent) power += (value - mean) * (value - mean);
        const double deviation = std::sqrt(power / double(beats));
        int best = 0;
        for (int place = 0; place < 4; ++place) {
            sums[place] /= double(std::max<size_t>(1, held[place]));
            if (sums[place] > sums[best]) best = place;
        }
        const double lean = deviation > 1e-9 ? (sums[best] - mean) / deviation : 0.;
        if (lean * std::sqrt(double(beats) / 4) >= kMinBarLean) {
            result.firstBar = best;
            result.barStrength = lean;
        }
    }
    return result;
}

Result detect(const float *mono, size_t count, int rate, const std::function<bool()> &cancelled, int threads) {
    Analysis analysis(rate, threads);
    // Handed over a minute at a time, so a cancel is seen while the spectra are made.
    const size_t block = size_t(std::clamp(rate, 8000, 48000)) * 60;
    for (size_t at = 0; at < count; at += block) {
        if (cancelled && cancelled()) {
            Result result;
            result.cancelled = true;
            return result;
        }
        analysis.push(mono + at, std::min(block, count - at));
    }
    return analysis.finish(cancelled);
}

}
