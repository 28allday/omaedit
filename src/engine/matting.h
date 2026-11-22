#pragma once

// Finding the people in a picture: MODNet (Ke et al., Apache-2.0; its
// licence is in src/engine/models/), trained on webcam portraits. Its
// weights ship in the program's resources, batch norms folded in. Both
// engines run the network from one plan of passes: nn.comp on the GPU and
// run() below on the CPU, the same arithmetic on the same numbers.

#include <cstdint>
#include <string>
#include <vector>

#include "frame.h"

namespace engine::matting {

// A full convolution's weights are kept eight output channels at a time,
// [cout / 8][cin][k][k][8] (padded with zeros), so a GPU thread reads the
// eight it needs as two vectors; a depthwise one's as [c][k][k].
struct Model {
    struct Tensor {
        std::string name;
        uint32_t offset = 0;      // into values
        std::vector<int> shape;
    };
    std::vector<float> values;
    std::vector<Tensor> tensors;
    bool ok = false;
    const Tensor *find(const std::string &name) const;
};
// Read once, the first time it is wanted; not ok when the resource is missing or damaged.
const Model &model();

enum Op : int32_t { Input, Conv, Sum, Resize, NormStats, NormApply, Pool, Scale, Output, NormMerge };
enum Activation : int32_t { None, Relu, Relu6, Sigmoid };

// One pass, laid out as nn.comp's push block. Tensors are planar floats in
// one arena, a channel's plane after another's; offsets count floats.
//  Input:     the source (inW × inH, linear, straight alpha) averaged over each
//             of outW × outH areas, display-encoded and mapped to −1..1, at out.
//  Conv:      in0..in2 (c0..c2 channels, read as one), k × k, stride, padding
//             pad, into cout channels at out; depthwise takes channel c from
//             channel c. Then the bias, the residual (at residual, unless −1)
//             and the activation, on channels below actUntil. With splits > 1
//             the input channels are shared out and each share's sums go to
//             partial, for Sum to finish.
//  NormStats: for channels actUntil..cout over outW × outH, in `splits`
//             chunks, each chunk's count, mean and squared deviations, to in1.
//  NormMerge: the chunks together: each channel's mean and 1/σ, to partial.
//  NormApply: those channels normalised, then Relu.
//  Resize:    bilinear, half-pixel centres, edges clamped (as PyTorch's).
//  Pool:      each of c0 channels' mean, to out.
//  Scale:     in0's channels (c0 of outW × outH) times in1's numbers, in place.
//  Output:    in0, the matte (outW × outH), to the matte picture.
struct Pass {
    int32_t op = 0, inW = 0, inH = 0, outW = 0, outH = 0;
    int32_t in0 = 0, c0 = 0, in1 = 0, c1 = 0, in2 = 0, c2 = 0;
    int32_t out = 0, cout = 0, k = 1, stride = 1, pad = 0, depthwise = 0, act = 0, actUntil = 0;
    int32_t weights = 0, bias = 0, residual = -1, splits = 1, partial = 0;
};
static_assert(sizeof(Pass) == 96, "matches nn.comp's push block");

struct Plan {
    int width = 0, height = 0;    // the network's input
    std::vector<Pass> passes;
    // The passes in `parts` runs of about equal work, each part's end; one
    // part a frame spreads the network over that many (see divide).
    std::vector<size_t> ends;
    size_t arena = 0;             // floats
    int32_t matte = 0;            // where the matte is left, width × height
    bool ok = false;
};
// For a network of width × height (multiples of 32; see matteSize in
// frame.h), and a source of srcW × srcH.
Plan plan(const Model &m, int width, int height, int srcW, int srcH, int parts = 1);


// The CPU twin: passes first..last of the plan run on `src` (all of them by
// default), the matte left in `arena` at plan.matte once the last has run.
void run(const Plan &p, const Model &m, const FrameF &src, std::vector<float> &arena, size_t first = 0, size_t last = ~size_t(0));

}  // namespace engine::matting
