#pragma once

// Persisted IDs: 0 retains the original producer's sampling. Never reinterpret
// old 0/1/2 values when introducing explicit recorded-picture selection.
namespace FrameSampling {
enum Mode { Original = 0, Blend = 1, OpticalFlow = 2, Floor = 3, Nearest = 4 };
inline bool valid(int mode) { return mode >= Original && mode <= Nearest; }
inline bool interpolates(int mode) { return mode == Blend || mode == OpticalFlow; }
inline bool explicitPictures(int mode) { return mode == Floor || mode == Nearest; }
}
