#pragma once

// Engines for the editor's renderers. One Vulkan device for the whole
// program, FFmpeg's, on the GPU the engine ranks first (or the one chosen):
// every engine works on it, taking turns on its queues, so pictures can pass
// between engines, decoders and encoders on that GPU without copies. Without
// a usable GPU, the CPU twin.

#include "engine.h"

#include <QString>
#include <memory>

struct AVBufferRef;

namespace render {
std::unique_ptr<engine::Engine> createEngine();
// True when `engine` has lost its GPU (engine::Engine::lost): it will never
// draw again, so its owner drops it, and whatever was made on it, and asks
// createEngine() for another. Telling this is what lets the next one come
// back: it works on a device of its own rather than the shared one the lost
// engine was on, and on the CPU twin when the GPU is lost again at once.
bool engineLost(const engine::Engine *engine);
// The GPU the engines use and why, or why none (for diagnostics).
QString engineDevice();
// FFmpeg's device for that GPU (owned here, until the application ends) and
// its Vulkan index; null without one (or once an engine on it was lost, or
// the application has ended). Decoders on it hand their pictures to the
// engines without copies.
AVBufferRef *sharedDevice(int *index);
// The Vulkan index of the GPU the engines draw on, found without opening it;
// below zero when the CPU draws. For the decoder check at start-up: a decoder
// on this GPU hands its pictures over without copies, one on another GPU has
// every picture brought down to memory and sent up again.
int engineGpu();
}
