// A loaded cartridge and everything that runs it: the machine, the picture it draws into, the
// sound it is drained into, and the picture's shape as the frontend was last told it.

#pragma once

#include "picture.h"

#include "snaggletooth/snes/snes.h"

#include <array>

namespace snaggletooth_libretro {

// The machine keeps a pointer to the observer, so a session is built in place and never moved.
struct Session {
  explicit Session(const snaggletooth::SnesConfig& config);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Runs the machine until the beam reaches the next frame's first line, so a frame drawn a line
  // longer, as an interlaced field is, is run whole.
  void runFrame();

  // Hands the frontend the picture the last frame made, telling it the new geometry first when
  // the picture's shape has changed.
  void showPicture();

  // Hands the frontend the sound the last frame made, every sample from the console's time up
  // to the frame's end.
  void playSound();

  Screen screen;
  snaggletooth::Snes machine;
  // Where the machine's sound is drained to each frame, in as many passes as it takes. A frame
  // makes about 533 samples at 60 Hz and 640 at 50 Hz, so one pass is the usual case.
  std::array<snaggletooth::StereoFrame, 1024> sound{};
  // The geometry the frontend was last told.
  unsigned shownWidth = kBaseWidth;
  unsigned shownHeight = kBaseHeight;
  unsigned shownLines = kBaseHeight;
};

}  // namespace snaggletooth_libretro
