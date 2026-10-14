#include "battery_save.h"

#include <algorithm>
#include <memory>

namespace snaggletooth_libretro {

BatterySave::BatterySave(const snaggletooth::Snes& machine) : bytes_(machine.state().sram) {}

void* BatterySave::data() noexcept { return bytes_.empty() ? nullptr : bytes_.data(); }

std::size_t BatterySave::size() const noexcept { return bytes_.size(); }

void BatterySave::passToMachine(snaggletooth::Snes& machine) {
  if (std::ranges::equal(bytes_, machine.state().sram)) return;
  // A machine's state is a quarter of a megabyte, so the copy is made on the heap.
  const auto state = std::make_unique<snaggletooth::SnesState>(machine.state());
  std::ranges::copy(bytes_, state->sram.begin());
  machine.restore(*state);
}

void BatterySave::copyFromMachine(const snaggletooth::Snes& machine) {
  std::ranges::copy(machine.state().sram, bytes_.begin());
}

}  // namespace snaggletooth_libretro
