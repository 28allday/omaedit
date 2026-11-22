#pragma once

// Track, bus and master inserts on plain float samples, one timeline frame
// at a time (the block size the editor has always processed them in: the
// denoiser's analysis grid and every filter's memory follow it). Each keeps
// its memory across consecutive frames and starts afresh after a jump.
//
// EQ bands, shelves, hum notches, high- and low-pass are biquads with the
// coefficients of FFmpeg's af_biquads (RBJ, direct form I, in double); the
// gate and the limiter follow FFmpeg's af_agate and af_alimiter (both
// LGPL-2.1-or-later). The compressor, de-esser and denoiser are the editor's own.

#include "dsp.h"

#include <QString>
#include <QVariantMap>
#include <memory>
#include <vector>

namespace sound {

class Insert {
public:
    virtual ~Insert() = default;
    // Afresh: no memory of earlier samples.
    virtual void reset(int channels, int rate) = 0;
    // `n` interleaved samples of `channels` channels, in place.
    virtual void process(float *data, int n, int channels, int rate) = 0;
    // A copy with the same memory of earlier samples (an interrupted render
    // puts its inserts back as they were).
    virtual std::unique_ptr<Insert> clone() const = 0;
    // The largest gain reduction in the last block, in dB (meters), and its
    // value at each sample of that block (left empty by inserts that reduce
    // no gain).
    double reduction = 0;
    std::vector<double> reductions;
};

// One mix map's "fx" list as inserts, in order (bypassed ones left out,
// wet/dry mixes wrapped round theirs). Unknown ids are skipped.
std::vector<std::unique_ptr<Insert>> insertsFor(const QVariantList &fx);
// An fx entry's parameters with the defaults filled in.
QVariantMap withDefaults(const QString &id, const QVariantMap &params);

// A clip's EQ preset ("eq": voice, bass-cut, ...) as inserts.
std::vector<std::unique_ptr<Insert>> presetInserts(const QString &preset);

}  // namespace sound
