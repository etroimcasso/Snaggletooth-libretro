#include "picture.h"

#include <algorithm>

namespace snaggletooth_libretro {

float aspectOf(unsigned lines, Aspect aspect) {
  if (aspect == Aspect::FourByThree) return 4.0f / 3.0f;
  return static_cast<float>(kBaseWidth) / static_cast<float>(lines);
}

Picture::Rows Picture::place(unsigned fieldWidth, unsigned fieldLines, bool interlaced,
                             unsigned parity) {
  lines = fieldLines;
  if (!interlaced || mode == Interlace::Off) {
    width = fieldWidth;
    height = fieldLines;
    bob_ = false;
    return Rows{.first = pixels.data(), .pitch = width};
  }
  const unsigned tall = fieldLines * 2u;
  if (fieldWidth != width || tall != height) {
    width = fieldWidth;
    height = tall;
    std::fill_n(pixels.begin(), static_cast<std::size_t>(width) * height, 0u);
  }
  bob_ = mode == Interlace::Bob;
  parity_ = parity & 1u;
  return Rows{.first = pixels.data() + static_cast<std::size_t>(parity_) * width,
              .pitch = static_cast<std::size_t>(width) * 2u};
}

void Picture::finish() {
  if (!bob_) return;
  for (unsigned line = 0; line < lines; ++line) {
    const std::uint32_t* from = pixels.data() + static_cast<std::size_t>(2u * line + parity_) * width;
    std::copy_n(from, width,
                pixels.data() + static_cast<std::size_t>(2u * line + (parity_ ^ 1u)) * width);
  }
}

void Screen::frame(const snaggletooth::VideoFrame& field) {
  const bool interlaced = machine != nullptr && machine->state().ppu.interlace();
  const Picture::Rows rows = into_.place(field.width, field.height, interlaced, field.field);
  const std::uint8_t* from = field.pixels.data();
  for (unsigned line = 0; line < field.height; ++line) {
    std::uint32_t* to = rows.first + static_cast<std::size_t>(line) * rows.pitch;
    for (unsigned x = 0; x < field.width; ++x, from += 4) {
      to[x] = (static_cast<std::uint32_t>(from[0]) << 16) |
              (static_cast<std::uint32_t>(from[1]) << 8) | static_cast<std::uint32_t>(from[2]);
    }
  }
  into_.finish();
  throw FrameFinished{};
}

void GpuScreen::frame(const snaggletooth::PpuTimeline& timeline) {
  const bool interlaced = machine != nullptr && machine->state().ppu.interlace();
  const Picture::Rows rows = into_.place(timeline.width, timeline.height, interlaced, timeline.field);
  if (hardware == nullptr && device != nullptr &&
      device->readBack(timeline.width, timeline.height, rows.first, rows.pitch)) {
    into_.finish();
  }
  throw FrameFinished{};
}

}  // namespace snaggletooth_libretro
