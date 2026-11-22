#pragma once

// OmaEdit's own sound mixer: a timeline's sound at 48 kHz in float, from the
// same description the picture engine draws from (entries, track flags with
// their mix maps, the master mix map, media settings).
//
// Samples are placed on the timeline's sample clock: frame f starts at
// llrint(f × rate / fps), fps as a float (the rule the editor has always
// used, so blocks line up with the frames they belong to). A block may be any
// range of samples.
//
// Each clip is read by AudioSampleSource::Reader and given its level, level
// keyframes, pan and fades; each track sums its clips and goes through its
// fader; tracks and buses route to buses and the master as the mix maps say
// (outputs, sends, mute, solo); the master fader comes last. What the mixer
// does not draw yet it says (unsupported()), and the caller reads that
// timeline's sound elsewhere.

#include "dsp.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace sound {

struct TimelineSound {
    QVariantList entries;   // as Timeline::entries()
    QVariantList tracks;    // as Timeline::trackFlagsVariant(), with "index"
    QVariantMap master;     // the master mix map
    QVariantMap mediaSources;
    int fpsNum = 25, fpsDen = 1;
};

// What a render reports for meters, as it goes (each call covers one stretch
// of samples, a timeline frame at most).
struct Taps {
    // A track's block at a stage: 0 its clips summed, 1 after its inserts,
    // 2 after its fader.
    std::function<void(int track, int stage, const Samples &data, int channels, int rate)> stage;
    // A track's (bus empty), a bus's or the master's ("master", no mix) block
    // after its fader, as it is routed, with its mix map and its inserts'
    // gain reduction at each sample (dB; empty when they reduce none).
    std::function<void(int track, const QString &bus, const Samples &post, int channels, int rate, const QVariantMap &mix,
                       const std::vector<double> &reductions)> route;
};

struct MixMemory;  // see Mixer::remember

class Mixer {
public:
    Mixer();
    ~Mixer();
    // Cheap when nothing changed (readers and their caches are kept by path).
    void configure(const TimelineSound &timeline);
    // Goes up each time configure() was given something other than it had:
    // sound rendered under one number was all made from one description.
    quint64 generation() const;
    // Empty when every audible clip, track and bus can be mixed here.
    QString unsupported() const;
    // `count` samples of `channels` channels at `rate` from timeline sample
    // `first`, interleaved; 8 to 384 kHz, one to eight channels.
    bool render(qint64 first, int count, int rate, int channels, Samples &out, QString *error, const Taps *taps = nullptr);
    // A frame's first sample and its sample count, on the timeline's clock.
    static qint64 frameStart(qint64 frame, double fps, int rate);
    static int frameSamples(qint64 frame, double fps, int rate);
    // Whether a clip of the timeline's list sounds in the mix at all: a sound
    // clip, and as much a picture that plays sound of its own (a nested
    // sequence, a compound clip, a clip that still holds its sound).
    static bool sounds(const QVariantMap &clip);

    // What a render found each insert chain holding before it ran it. While
    // remembering, takeMemory() after a render gives that render's; recall()
    // puts those chains back as they were then, wherever the layout still has
    // the chain, made from the same list. Recalling the memories of
    // consecutive renders, newest first, puts the mixer back to before the
    // oldest of them, and the samples from there come out as they did the
    // first time. (A stretched clip's stretcher starts again where it is next
    // read, and a nested sequence's own inserts are not remembered: both begin
    // afresh, as they do after any jump.)
    void remember(bool on);
    std::shared_ptr<const MixMemory> takeMemory();
    void recall(const MixMemory &memory);

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace sound
