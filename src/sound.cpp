#include "sound.h"

#include "frontend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace snaggletooth_libretro {

// The frontend takes sound as 16-bit signed pairs, left then right, which is the layout of the
// machine's StereoFrame, so the machine's samples are handed over as they are.
static_assert(std::is_standard_layout_v<snaggletooth::StereoFrame>);
static_assert(sizeof(snaggletooth::StereoFrame) == 2u * sizeof(std::int16_t));
static_assert(offsetof(snaggletooth::StereoFrame, left) == 0u);
static_assert(offsetof(snaggletooth::StereoFrame, right) == sizeof(std::int16_t));

void handOver(std::span<const snaggletooth::StereoFrame> frames) {
  if (audioSampleBatch == nullptr) return;
  const auto* samples = reinterpret_cast<const std::int16_t*>(frames.data());
  std::size_t untaken = frames.size();
  while (untaken > 0u) {
    const std::size_t taken = std::min(audioSampleBatch(samples, untaken), untaken);
    if (taken == 0u) return;
    samples += taken * 2u;
    untaken -= taken;
  }
}

}  // namespace snaggletooth_libretro
