// The core's options, as the frontend lists them under Core Options, and the choices the player
// made in them.

#pragma once

#include "picture.h"

namespace snaggletooth_libretro {

// Lists the core's options with the frontend in the newest form it takes: definitions grouped
// into categories (version 2), plain definitions (version 1), or, for any frontend, one
// "Description; value|value" line for each option, its default value first.
void registerOptions();

// Whether the frontend reports that the player changed an option since it was last asked.
bool optionsChanged();

// The interlace presentation the player chose, or Weave where the frontend has no choice for it.
Interlace interlaceOption();

}  // namespace snaggletooth_libretro
