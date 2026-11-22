#pragma once
#include <array>
#include <QtGlobal>
namespace SuiteProcessing {
constexpr int schemaVersion=1;
// Three row-major RGB rows, each containing RGB coefficients and an offset.
// Straight coverage is untouched; coefficients and evaluation use double,
// with one float store per output channel. RGB is never clamped.
struct RgbAffine {
    std::array<double,12> values{{1,0,0,0,0,1,0,0,0,0,1,0}};
    bool valid() const;
};
// Caller owns a contiguous RGBA32F span of count pixels. No allocation or copy.
// Invalid arguments/coefficient values fail before modifying the span.
bool applyAffineCpu(float *rgba,qsizetype count,const RgbAffine &transform);
}
