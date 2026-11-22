#include "dsp.h"

#include <algorithm>
#include <cmath>

namespace sound {

namespace {
double number(const QVariantMap &m, const QString &key, double fallback = 0) {
    const double v = m.value(key, fallback).toDouble();
    return std::isfinite(v) ? v : fallback;
}
}  // namespace

double gain(double db) {
    return db <= -100 ? 0 : std::pow(10., std::clamp(db, -100., 36.) / 20.);
}

QVariantList busList(const QVariantMap &master) {
    return master.value("buses", QVariantList{QVariantMap{{"id", "bus1"}, {"name", "Bus 1"}, {"mix", QVariantMap()}}}).toList();
}

Curve::Curve(const QVariantMap &mix, const QString &key, double fallback) : fallback(fallback) {
    off = mix.value("automation").toString() == "off";
    if (off) return;
    for (const auto &v : mix.value("automationKeys").toMap().value(key).toList()) {
        const auto k = v.toMap();
        const QString interpolation = k.value("interpolation").toString();
        keys.push_back({number(k, "frame"), number(k, "value", fallback), k.isEmpty(),
                        interpolation == "hold", interpolation == "smooth"});
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key &a, const Key &b) { return a.frame < b.frame; });
}

double Curve::at(double frame) const {
    if (off) return fallback;
    // The key at or before the frame and the one at or after it; of several
    // keys on one frame, the one listed last.
    const auto after = std::upper_bound(keys.begin(), keys.end(), frame, [](double f, const Key &k) { return f < k.frame; });
    const Key *a = after == keys.begin() ? nullptr : &*(after - 1);
    const Key *b = nullptr;
    if (a && a->frame == frame) b = a;
    else if (after != keys.end())
        b = &*(std::upper_bound(after, keys.end(), after->frame, [](double f, const Key &k) { return f < k.frame; }) - 1);
    if (!a || a->empty) return !b || b->empty ? fallback : b->value;
    if (!b || b->empty || a->frame == b->frame || a->hold) return a->value;
    double t = (frame - a->frame) / (b->frame - a->frame);
    if (a->smooth) t = t * t * (3 - 2 * t);
    return a->value * (1 - t) + b->value * t;
}

Fader::Fader(const QVariantMap &mix)
    : muted(mix.value("muted").toBool()), volume(mix, "volume", number(mix, "volume")), pan(mix, "pan", number(mix, "pan")) {
    for (const auto &v : mix.value("normalizations").toList()) {
        const auto r = v.toMap();
        normalizations.push_back({number(r, "in"), number(r, "out"), number(r, "gain")});
    }
}

void Fader::run(Samples &data, int ch, int rate, double fps, double pos) const {
    if (muted) {
        std::fill(data.begin(), data.end(), 0);
        return;
    }
    for (size_t i = 0; i < data.size() / size_t(ch); ++i) {
        const double frame = pos + double(i) * fps / rate;
        const double db = volume.at(frame);
        const double panning = std::clamp(pan.at(frame), -1., 1.);
        double offset = 0;
        for (const Range &r : normalizations)
            if (frame >= r.in && frame < r.out + 1) offset += r.gain;
        const double g = gain(db + offset);
        for (int c = 0; c < ch; ++c) {
            // Stereo balance: center is unity; only the opposite side attenuates.
            const double balance = ch < 2 ? 1 : c == 0 ? std::min(1., 1. - panning) : c == 1 ? std::min(1., 1. + panning) : 1.;
            data[i * size_t(ch) + size_t(c)] *= g * balance;  // in double, as it has always been
        }
    }
}

void fader(Samples &data, int ch, int rate, double fps, double pos, const QVariantMap &mix) {
    Fader(mix).run(data, ch, rate, fps, pos);
}

double fadeGain(const QString &shape, double x) {
    return shape == "equalPower" ? std::sin(x * M_PI / 2)
           : shape == "smooth"   ? x * x * (3 - 2 * x)
           : shape == "linear"   ? x
                                 : gain(-60 + 60 * x);
}

}  // namespace sound
