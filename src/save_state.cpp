#include "save_state.h"

#include "frontend.h"

namespace snaggletooth_libretro {

SaveState::SaveState(const snaggletooth::Snes& machine)
    : read_(std::make_unique<snaggletooth::SnesState>()) {
  read_->sram.resize(machine.state().sram.size());
}

std::size_t SaveState::size(const snaggletooth::Snes& machine) noexcept {
  return machine.state().byteSize();
}

bool SaveState::save(const snaggletooth::Snes& machine, std::span<std::uint8_t> out) noexcept {
  return machine.state().asBytes(out) != 0u;
}

bool SaveState::load(snaggletooth::Snes& machine, std::span<const std::uint8_t> bytes) {
  const std::size_t needed = size(machine);
  if (bytes.size() < needed) return false;
  if (!snaggletooth::SnesState::fromBytes(bytes.first(needed), *read_)) return false;
  return machine.restore(*read_);
}

void declareSerializationQuirks() {
  std::uint64_t quirks = 0u;
  if (environment != nullptr) environment(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
}

}  // namespace snaggletooth_libretro
