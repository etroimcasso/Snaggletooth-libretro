// The cartridge's save as the frontend keeps it: one buffer the frontend reads and writes
// directly, as libretro's RETRO_MEMORY_SAVE_RAM, kept in step with the save inside the machine.

#pragma once

#include "snaggletooth/snes/snes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace snaggletooth_libretro {

// The machine holds a cartridge's save in its state and takes a new one only as part of a
// restored state, so the frontend is handed a buffer of the core's own. The frontend fills it
// from its save file after a cartridge loads and reads it back whenever it writes the file; the
// core carries a change the frontend makes into the machine as the next frame begins, and the
// machine's save into the buffer as each frame ends. Between frames the buffer holds the save as
// it stood at the end of the frame last handed over.
class BatterySave {
 public:
  // A buffer the size of the save the machine was built with, holding what the machine holds:
  // the bytes the cartridge's header declares, or none for a cartridge that declares no save.
  explicit BatterySave(const snaggletooth::Snes& machine);
  BatterySave(const BatterySave&) = delete;
  BatterySave& operator=(const BatterySave&) = delete;

  // The buffer the frontend reads and writes, or null for a cartridge without a save. It stays
  // where it is for as long as the cartridge is loaded.
  [[nodiscard]] void* data() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

  // Puts the buffer into the machine when it differs from the machine's save, which is when the
  // frontend has written it. The machine takes it through a restore of its state as it stands,
  // with the buffer as the save, so everything else, the pads in the ports included, stays as it
  // was. Called between frames.
  void passToMachine(snaggletooth::Snes& machine);

  // Copies the machine's save into the buffer. Called after each frame.
  void copyFromMachine(const snaggletooth::Snes& machine);

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace snaggletooth_libretro
