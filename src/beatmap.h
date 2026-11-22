#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// Finds the beats of a piece of music: its tempo and the time of each beat.
//
// The sound is taken as one channel at a low rate. A short-time spectrum gives
// an onset strength (how much louder each band has just become, on a
// logarithmic scale); the autocorrelation of that, weighted towards the tempi
// people tap to, gives the beat's period; and a dynamic programme then places
// the beats where the onsets are while keeping them that period apart, so they
// follow the playing and a tempo that drifts a little. The bar is taken as
// four beats, and its first beat is the one of the four that the music leans
// on: the harder attacks, the low notes, the changes of chord.
//
// Nothing here knows about the editor: samples in, times out. The same samples
// give the same answer however they are handed over and on however many
// threads.
namespace BeatMap {

struct Result {
    double bpm = 0;            // 0: no steady beat was found
    std::vector<double> beats; // seconds from the first sample, ascending
    // How regular the onsets are at the beat's period, 0 to 1 (the
    // autocorrelation there against the onsets' own power). Sound with no
    // beat stays low and gives no beats.
    double strength = 0;
    // The bar: how many beats it has, and which of `beats` is the first beat
    // of a bar (0 to beatsPerBar - 1; every beatsPerBar-th from it is one).
    // Music that leans on no beat more than the others counts from its first.
    int beatsPerBar = 4;
    int firstBar = 0;
    // How far the chosen beat stands above the other three, in deviations of
    // the beats' accents; 0 where nothing told them apart.
    double barStrength = 0;
    bool cancelled = false;
};

// The rate the sound is analysed at; other rates between 8 and 48 kHz work
// (the frame is sized to them), this is the one the editor decodes to.
inline constexpr int kRate = 11025;

// Sound handed over a block at a time, so a long recording is never held whole.
class Analysis {
public:
    // `threads`: how many the spectra are made on; 0 takes what the machine has (8 at most).
    explicit Analysis(int rate = kRate, int threads = 0);
    ~Analysis();
    Analysis(const Analysis &) = delete;
    Analysis &operator=(const Analysis &) = delete;
    void push(const float *mono, size_t count);
    // `cancelled` is asked from time to time; a cancelled result has no beats.
    Result finish(const std::function<bool()> &cancelled = {});

private:
    struct State;
    std::unique_ptr<State> d;
};

Result detect(const float *mono, size_t count, int rate = kRate,
              const std::function<bool()> &cancelled = {}, int threads = 0);

}
