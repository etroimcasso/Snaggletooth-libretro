// The picture as the frontend is handed it: its shapes and aspect, how a game's interlaced
// fields are shown, and the two observers that turn each finished frame into XRGB8888 pixels,
// one from the frame the machine drew on the processor and one from the frame it drew on the GPU.

#pragma once

#include "gpu_device.h"

#include "snaggletooth/snes/ppu_timeline.h"
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

// The shape the frontend scales the picture to. SquarePixels is that of square pixels 256
// across, over the lines the console draws (224, or 239 for the taller picture); FourByThree is
// the television's 4:3 whatever the lines. A frame in half-pixels and a woven one keep the
// aspect of the lines drawn, so the picture holds one shape on screen whatever size the frame is.
enum class Aspect : std::uint8_t { SquarePixels, FourByThree };

float aspectOf(unsigned lines, Aspect aspect);

// How a game's interlaced fields are shown. The machine draws one field a frame, alternating
// parity: Weave interleaves the last two into the full-height picture the console sent, Bob
// shows each field at full height on its own, and Off shows each field as it is drawn.
enum class Interlace : std::uint8_t { Weave, Bob, Off };

// Thrown by either observer as the machine finishes a frame. The machine returns from run()
// through it with the beam where the frame's vertical blank begins, ahead of the blank's
// interrupt handler and controller read, which is where a frontend's frame ends; the next run()
// spends the rest of the interrupted budget before its own.
struct FrameFinished {};

// The picture the frontend is handed: XRGB8888, one 32-bit 0x00RRGGBB word a pixel, row-major
// from the top line, in a buffer sized once for the largest picture, so no frame allocates.
//
// A frame the cartridge did not interlace, or any frame under Off, is the picture as drawn. A
// field of an interlaced run is placed in the full-height picture at the rows of its parity, line
// i at row 2i + parity: under Weave the other parity's rows keep the last field of that parity,
// and under Bob this field fills them too. A picture of a new shape starts black. The placement
// is the same whichever way the frame was drawn, so the picture does not change when the drawing
// moves between the processor and the GPU.
class Picture {
 public:
  // Where a field's lines are written: line i at `first + i * pitch`.
  struct Rows {
    std::uint32_t* first;
    std::size_t pitch;
  };

  // Makes the picture ready for a field `fieldWidth` across and `fieldLines` down, of the given
  // parity, and answers where its lines go.
  Rows place(unsigned fieldWidth, unsigned fieldLines, bool interlaced, unsigned parity);
  // Completes the field place() made ready for, once its lines are written: under Bob each line
  // is copied onto the other parity's row.
  void finish();

  Interlace mode = Interlace::Weave;
  std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(
      static_cast<std::size_t>(kMaxWidth) * kMaxHeight, 0u);
  unsigned width = kBaseWidth;
  unsigned height = kBaseHeight;
  unsigned lines = kBaseHeight;  // the lines the console drew the last frame with

 private:
  bool bob_ = false;     // the field being placed is copied onto the other parity's rows
  unsigned parity_ = 0;  // the field's parity
};

// The machine's frame observer, for a machine drawing on the processor: it converts each
// finished frame's RGBA bytes into the picture, then stops the run there.
class Screen final : public snaggletooth::FrameObserver {
 public:
  explicit Screen(Picture& into) : into_(into) {}
  void frame(const snaggletooth::VideoFrame& field) override;

  // The machine whose PPU says whether the run is interlaced.
  const snaggletooth::Snes* machine = nullptr;

 private:
  Picture& into_;
};

class HardwareDevice;

// The machine's timeline observer, for a machine drawing on the GPU: the device has drawn the
// frame by the time the machine reports it, then the run stops there. On the core's own device
// the observer reads the frame back into the picture; on the frontend's device the frame stays
// in the device's texture, already placed, and the observer records only the picture's shape.
class GpuScreen final : public snaggletooth::PpuTimelineObserver {
 public:
  explicit GpuScreen(Picture& into) : into_(into) {}
  void frame(const snaggletooth::PpuTimeline& timeline) override;

  // The machine whose PPU says whether the run is interlaced, and the device it draws on: the
  // core's own, or the frontend's.
  const snaggletooth::Snes* machine = nullptr;
  GpuDevice* device = nullptr;
  HardwareDevice* hardware = nullptr;

 private:
  Picture& into_;
};

}  // namespace snaggletooth_libretro
