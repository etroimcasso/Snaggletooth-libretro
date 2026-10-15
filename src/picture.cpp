#include "picture.h"

#include <algorithm>

namespace snaggletooth_libretro {

float aspectOf(unsigned lines, Aspect aspect) {
  if (aspect == Aspect::FourByThree) return 4.0f / 3.0f;
  return static_cast<float>(kBaseWidth) / static_cast<float>(lines);
}

void Screen::frame(const snaggletooth::VideoFrame& field) {
  const bool interlaced = machine != nullptr && machine->state().ppu.interlace();
  lines = field.height;
  if (!interlaced || mode == Interlace::Off) {
    width = field.width;
    height = field.height;
    for (unsigned line = 0; line < field.height; ++line) convertLine(field, line, line);
  } else {
    const unsigned tall = field.height * 2u;
    if (field.width != width || tall != height) {
      width = field.width;
      height = tall;
      std::fill_n(pixels.begin(), static_cast<std::size_t>(width) * height, 0u);
    }
    const unsigned parity = field.field & 1u;
    for (unsigned line = 0; line < field.height; ++line) {
      convertLine(field, line, 2u * line + parity);
      if (mode == Interlace::Bob) {
        std::copy_n(rowAt(2u * line + parity), width, rowAt(2u * line + (parity ^ 1u)));
      }
    }
  }
  throw FrameFinished{};
}

std::uint32_t* Screen::rowAt(unsigned row) {
  return pixels.data() + static_cast<std::size_t>(row) * width;
}

void Screen::convertLine(const snaggletooth::VideoFrame& field, unsigned line, unsigned row) {
  const std::uint8_t* from =
      field.pixels.data() + static_cast<std::size_t>(line) * field.width * 4u;
  std::uint32_t* to = rowAt(row);
  for (unsigned x = 0; x < field.width; ++x, from += 4) {
    to[x] = (static_cast<std::uint32_t>(from[0]) << 16) |
            (static_cast<std::uint32_t>(from[1]) << 8) | static_cast<std::uint32_t>(from[2]);
  }
}

}  // namespace snaggletooth_libretro
