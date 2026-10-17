// The machine's state as the frontend saves and loads it: the bytes of libretro's save states,
// which the frontend also takes for rewind, run-ahead and netplay.

#pragma once

#include "snaggletooth/snes/snes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace snaggletooth_libretro {

// The library's own byte form of the machine's state, SnesState::asBytes(): the same bytes for
// the same state on every platform, and the same size for the whole life of a loaded cartridge,
// since the save's size is the cartridge's. A state saved and loaded back puts the machine on
// the cycle it was saved at, so the frames and sound after a load are the ones the machine made
// after the save. The picture the frontend is shown is the core's, not the machine's, and is not
// part of the state: under Weave, the first frame of an interlaced picture after a load has the
// other field's rows from the last frame shown, until the next field replaces them.
class SaveState {
 public:
  // Ready to load the states of the machine's cartridge.
  explicit SaveState(const snaggletooth::Snes& machine);
  SaveState(const SaveState&) = delete;
  SaveState& operator=(const SaveState&) = delete;

  // The bytes a state of the machine takes: 268,858 and the save's size.
  [[nodiscard]] static std::size_t size(const snaggletooth::Snes& machine) noexcept;

  // Writes the machine's state into the first size() bytes of `out`, allocating nothing.
  // Answers false, having written nothing, when `out` is shorter. Called between frames.
  [[nodiscard]] static bool save(const snaggletooth::Snes& machine,
                                 std::span<std::uint8_t> out) noexcept;

  // Reads a state from the first size() bytes of `bytes` and puts it into the machine. Answers
  // false, leaving the machine as it was, when `bytes` is shorter, when they are not a state the
  // library wrote, or when their save is another size than the machine's, which makes them a
  // state of another cartridge. Called between frames.
  [[nodiscard]] bool load(snaggletooth::Snes& machine, std::span<const std::uint8_t> bytes);

 private:
  // The state the bytes are read into. A state is a quarter of a megabyte, so it is kept on the
  // heap, and with its save already the machine's size a load allocates nothing.
  std::unique_ptr<snaggletooth::SnesState> read_;
};

// Tells the frontend the state has none of libretro's serialization quirks: it is complete and
// deterministic, ready from the moment a cartridge loads, one size for the cartridge, and loads
// in any later session and on any platform.
void declareSerializationQuirks();

}  // namespace snaggletooth_libretro
