// The picture as the frontend is handed it: its shapes and aspect, how a game's interlaced
// fields are shown, and the frame observer that turns each finished frame into XRGB8888 pixels.

#pragma once

#include "snaggletooth/snes/snes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace snaggletooth_libretro {

// The picture's shapes: 256 by 224 to begin with, and at its largest 512 across, where a frame
// is drawn in half-pixels, and 478 down, where two 239-line fields are woven into one picture.
constexpr unsigned kBaseWidth = 256u;
constexpr unsigned kBaseHeight = 224u;
constexpr unsigned kMaxWidth = 512u;
constexpr unsigned kMaxHeight = 478u;

// The picture's aspect is that of square pixels 256 across, over the lines the console draws
// (224, or 239 for the taller picture). A frame in half-pixels and a woven one keep that same
// aspect, so the picture holds one shape on screen whatever size the frame is.
float aspectOf(unsigned lines);

// How a game's interlaced fields are shown. The machine draws one field a frame, alternating
// parity: Weave interleaves the last two into the full-height picture the console sent, Bob
// shows each field at full height on its own, and Off shows each field as it is drawn.
enum class Interlace : std::uint8_t { Weave, Bob, Off };

// Thrown by the frame observer as the machine finishes a frame. The machine returns from
// run() through it with the beam at the next frame's first line, which is where a frontend's
// frame ends; the next run() spends the rest of the interrupted budget before its own.
struct FrameFinished {};

// The machine's frame observer. It turns each finished frame into the picture the frontend is
// handed, then stops the run there. The picture is XRGB8888, one 32-bit 0x00RRGGBB word a
// pixel, row-major from the top line, in a buffer sized once for the largest picture, so no
// frame allocates. A frame the cartridge did not interlace, or any frame under Off, is the
// picture as drawn. A field of an interlaced run is placed in the full-height picture at the
// rows of its parity, line i at row 2i + parity: under Weave the other parity's rows keep the
// last field of that parity, and under Bob this field fills them too. A picture of a new shape
// starts black.
class Screen final : public snaggletooth::FrameObserver {
 public:
  void frame(const snaggletooth::VideoFrame& field) override;

  // The machine whose PPU says whether the run is interlaced.
  const snaggletooth::Snes* machine = nullptr;
  Interlace mode = Interlace::Weave;

  std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(
      static_cast<std::size_t>(kMaxWidth) * kMaxHeight, 0u);
  unsigned width = kBaseWidth;
  unsigned height = kBaseHeight;
  unsigned lines = kBaseHeight;  // the lines the console drew the last frame with

 private:
  [[nodiscard]] std::uint32_t* rowAt(unsigned row);
  void convertLine(const snaggletooth::VideoFrame& field, unsigned line, unsigned row);
};

}  // namespace snaggletooth_libretro
