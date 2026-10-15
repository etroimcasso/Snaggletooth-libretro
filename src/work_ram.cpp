#include "work_ram.h"

#include <algorithm>

namespace snaggletooth_libretro {

WorkRam::WorkRam(const snaggletooth::Snes& machine) : bytes_(machine.state().wram) {}

void WorkRam::passToMachine(snaggletooth::Snes& machine) {
  const auto& held = machine.state().wram;
  if (std::ranges::equal(bytes_, held)) return;
  for (std::size_t at = 0; at < bytes_.size(); ++at) {
    if (bytes_[at] != held[at]) {
      machine.poke(kWorkRamStart + static_cast<std::uint32_t>(at), bytes_[at]);
    }
  }
}

void WorkRam::copyFromMachine(const snaggletooth::Snes& machine) {
  std::ranges::copy(machine.state().wram, bytes_.begin());
}

}  // namespace snaggletooth_libretro
