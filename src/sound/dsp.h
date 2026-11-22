#pragma once

// The editor's sound maths on plain float samples:
// gains, automation curves, the track/bus/master fader and the fade shapes.
// Interleaved float samples throughout; `fps` and frame positions are the
// timeline's, so automation keys (in frames) land on the right samples.

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

namespace sound {

using Samples = std::vector<float>;

// dB to an amplitude factor; -100 dB and below is silence.
double gain(double db);

// A master mix map's buses. A project whose bus list was never edited has one,
// "Bus 1" (id "bus1"): the Audio page offers it and routing to it is valid,
// so the mixer has it too.
QVariantList busList(const QVariantMap &master);

// One automation lane of a mix map ("automationKeys" → key), read once so a
// per-sample evaluation is arithmetic on plain numbers. "automation": "off"
// gives the fallback everywhere. Keys interpolate linearly, hold, or smooth
// (smoothstep) to the next. The keys are kept in frame order (keys of one
// frame in the order they were listed), so a value is found by bisection
// however many keys a long automation pass has left.
struct Curve {
    struct Key {
        double frame = 0, value = 0;
        bool empty = false, hold = false, smooth = false;
    };
    std::vector<Key> keys;
    double fallback = 0;
    bool off = false;
    Curve() = default;
    Curve(const QVariantMap &mix, const QString &key, double fallback);
    double at(double frame) const;
};

// A mix map's fader, read once: mute, the volume and pan lanes, the
// normalisation ranges. The mixer keeps one for each strip while its mix map
// stays the same, so a block costs no reading of the map.
struct Fader {
    struct Range { double in, out, gain; };
    bool muted = false;
    Curve volume, pan;
    std::vector<Range> normalizations;
    Fader() = default;
    explicit Fader(const QVariantMap &mix);
    // On `data` (`ch` channels at `rate`), its first sample at timeline frame
    // `pos`: volume and pan automation (balance law: centre is unity, only the
    // opposite side attenuates), normalisation ranges.
    void run(Samples &data, int ch, int rate, double fps, double pos) const;
};

// A mix map's fader on `data`, read from the map there and then.
void fader(Samples &data, int ch, int rate, double fps, double pos, const QVariantMap &mix);

// A fade's gain at `x` (0 → 1 across the fade, the incoming direction):
// "equalPower", "smooth", "linear", or the default −60 dB → 0 dB ramp.
double fadeGain(const QString &shape, double x);

}  // namespace sound
