// The core's options, as the frontend lists them under Core Options, and the choices the player
// made in them.

#pragma once

#include "picture.h"
#include "session.h"

#include "snaggletooth/snes/snes.h"

#include <optional>

namespace snaggletooth_libretro {

// Lists the core's options with the frontend in the newest form it takes: definitions grouped
// into categories (version 2), plain definitions (version 1), or, for any frontend, one
// "Description; value|value" line for each option, its default value first.
void registerOptions();

// Whether the frontend reports that the player changed an option since it was last asked.
bool optionsChanged();

// Each option as the player set it, or its default where the frontend has no choice for it.
// The interlace presentation defaults to Weave and the aspect to square pixels. The region is
// std::nullopt under Auto, the default, where the cartridge's header names it. The machine runs
// its picture on a thread of its own and its sound on the frontend's thread by default.
Interlace interlaceOption();
Aspect aspectOption();
std::optional<snaggletooth::Region> regionOption();
MachineThreads threadsOption();

}  // namespace snaggletooth_libretro
