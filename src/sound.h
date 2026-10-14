// The console's sound as the frontend is handed it: the rate it plays at, and the hand-off of
// the samples each frame makes.

#pragma once

#include "snaggletooth/apu/dsp.h"

#include <span>

namespace snaggletooth_libretro {

// The rate of the console's sound: the audio machine runs 1,024,000 cycles to a second of the
// console's time, in both regions, and makes one stereo sample every 32 of them.
constexpr double kSampleRate = 32000.0;

// Hands stereo samples to the frontend, calling again with whatever a call leaves untaken. A
// frontend that takes none of a call's samples is not asked for the rest.
void handOver(std::span<const snaggletooth::StereoFrame> frames);

}  // namespace snaggletooth_libretro
