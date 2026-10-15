// The console's work RAM as the frontend keeps it: one buffer the frontend reads and writes
// directly, as libretro's RETRO_MEMORY_SYSTEM_RAM, kept in step with the work RAM inside the
// machine.

#pragma once

#include "snaggletooth/snes/snes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace snaggletooth_libretro {

// The console's 128 KB of work RAM, and the bus address its first byte has: banks $7E-$7F.
constexpr std::size_t kWorkRamBytes = std::tuple_size_v<decltype(snaggletooth::SnesState::wram)>;
constexpr std::uint32_t kWorkRamStart = 0x7E0000u;

// The machine holds its work RAM in its state and takes a byte from outside only through a
// poke, so the frontend is handed a buffer of the core's own, which it reads for achievements
// and its own cheat search and may write. The core carries a change the frontend makes into
// the machine as the next frame begins, and the machine's work RAM into the buffer as each
// frame ends. Between frames the buffer holds the work RAM as it stood at the end of the frame
// last handed over.
class WorkRam {
 public:
  // A buffer holding what the machine holds.
  explicit WorkRam(const snaggletooth::Snes& machine);
  WorkRam(const WorkRam&) = delete;
  WorkRam& operator=(const WorkRam&) = delete;

  // The buffer the frontend reads and writes. It stays where it is for as long as the cartridge
  // is loaded.
  [[nodiscard]] void* data() noexcept { return bytes_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  // Pokes every byte of the buffer that differs from the machine's work RAM into the machine,
  // which is each byte the frontend has written. A frame with no write costs one compare.
  // Called between frames.
  void passToMachine(snaggletooth::Snes& machine);

  // Copies the machine's work RAM into the buffer. Called after each frame.
  void copyFromMachine(const snaggletooth::Snes& machine);

 private:
  std::array<std::uint8_t, kWorkRamBytes> bytes_;
};

}  // namespace snaggletooth_libretro
