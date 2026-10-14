// The cartridge as the frontend hands it over: its image, the region it was made for, and the
// chip on a board the core refuses.

#pragma once

#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace snaggletooth_libretro {

// The cartridge image in the content the frontend read: the content as it is, or what follows a
// copier's header when one leads it.
std::vector<std::uint8_t> cartridgeImage(std::span<const std::uint8_t> content);

// The region a cartridge was made for, read from its header's country byte. A cartridge whose
// header names no region runs at 60 Hz.
snaggletooth::Region regionOf(std::span<const std::uint8_t> rom);

// The chip a cartridge's header names, as a player would know it.
const char* chipName(snaggletooth::Coprocessor chip);

}  // namespace snaggletooth_libretro
