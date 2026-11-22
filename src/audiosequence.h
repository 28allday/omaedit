#pragma once
#include <QtGlobal>

namespace AudioSequence {
// Exact seconds, independent of video frames and the eventual output sample rate.
struct Time {
    qint64 ticks = 0;
    qint64 rate = 1;
};
}
