// The console's two controller ports as the frontend fills them: the device in each, the
// buttons held on it, and the names the frontend shows for them.

#pragma once

#include "snaggletooth/snes/snes.h"

#include <optional>

namespace snaggletooth_libretro {

constexpr unsigned kPortCount = 2u;

// Puts the console's pad in both ports, as libretro takes a RetroPad to be plugged into every
// port until the frontend chooses otherwise, and asks the frontend whether it reports all of a
// pad's buttons in one call, as a bitmask.
void startControllers();

// Puts a device in one of the console's two ports: the console's pad for RETRO_DEVICE_JOYPAD,
// and an empty port for RETRO_DEVICE_NONE or any device the core does not offer. A port past
// the second is ignored. A choice holds across resets and cartridges until the frontend makes
// another or starts the core again.
void plugIn(unsigned port, unsigned device);

// Tells the frontend the devices each port takes, the console's pad or nothing, and the name of
// every button on both ports, as it shows them when the player binds their controls.
void describeControllers();

// What the frontend says is plugged into a port and held on it: the twelve buttons as the
// frontend reports them, or nothing for a port whose device is not a pad. The buttons are
// passed through as reported, so the machine sees every combination the frontend sends.
std::optional<snaggletooth::Joypad> heldOn(unsigned port);

}  // namespace snaggletooth_libretro
