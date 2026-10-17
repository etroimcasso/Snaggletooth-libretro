// A loaded cartridge and everything that runs it: the machine, the picture it draws into, the
// sound it is drained into, the save and work RAM the frontend keeps with the map that places
// them on the bus, the machine's saved states, and the picture's shape as the frontend was last
// told it.

#pragma once

#include "battery_save.h"
#include "memory_map.h"
#include "picture.h"
#include "save_state.h"
#include "work_ram.h"

#include "snaggletooth/snes/snes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace snaggletooth_libretro {

// Where the machine runs its sound unit's work and its picture's: on the thread the frontend
// calls the core on, or each on a thread of its own beside it. The sound, the picture and the
// state are the same either way.
struct MachineThreads {
  bool audio = false;
  bool picture = true;

  [[nodiscard]] bool operator==(const MachineThreads&) const noexcept = default;
};

// The machine keeps a pointer to the observer and the session's buffers are handed to the
// frontend, so a session is built in place and never moved.
struct Session {
  Session(std::vector<std::uint8_t> image, snaggletooth::Region region, MachineThreads threads);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Runs the machine until the beam reaches the next frame's first line, so a frame drawn a line
  // longer, as an interlaced field is, is run whole, then copies the machine's save and work RAM
  // into the frontend's buffers.
  void runFrame();

  // Hands the frontend the picture the last frame made, telling it the new geometry first when
  // the picture's shape or aspect has changed.
  void showPicture();

  // Hands the frontend the sound the last frame made, every sample from the console's time up
  // to the frame's end.
  void playSound();

  // Moves the machine's work onto the threads asked for. The machine takes its threads when it
  // is built, so a change builds a new machine on the same cartridge and restores the running
  // one's state into it: the game carries on from the cycle it stopped at, with the same pads,
  // save and work RAM.
  void runOn(MachineThreads threads);

  // Puts a saved state into the machine (SaveState::load), then copies the machine's save and
  // work RAM into the frontend's buffers, so they hold the state's and the next frame carries
  // no change of the frontend's into the machine. Answers false, with the machine and both
  // buffers as they were, when the state is refused.
  bool loadState(std::span<const std::uint8_t> bytes);

  // The cartridge image the machine was built from, and the configuration it was built with.
  std::vector<std::uint8_t> rom;
  snaggletooth::SnesConfig config;
  Screen screen;
  std::unique_ptr<snaggletooth::Snes> machine;
  BatterySave save{*machine};
  WorkRam workRam{*machine};
  MemoryMap memoryMap{*machine, workRam.data(), save.data()};
  SaveState states{*machine};
  // Where the machine's sound is drained to each frame, in as many passes as it takes. A frame
  // makes about 533 samples at 60 Hz and 640 at 50 Hz, so one pass is the usual case.
  std::array<snaggletooth::StereoFrame, 1024> sound{};
  // The aspect the player chose, and the geometry the frontend was last told.
  Aspect aspect = Aspect::SquarePixels;
  unsigned shownWidth = kBaseWidth;
  unsigned shownHeight = kBaseHeight;
  unsigned shownLines = kBaseHeight;
  Aspect shownAspect = Aspect::SquarePixels;
};

}  // namespace snaggletooth_libretro
