// libretro's hardware-render context: the frontend's own graphics device, asked for as a
// cartridge loads, and the core's hardware device made over it once the frontend has the context
// ready. The context asked for is the one the platform's backend serves (hardware_device.h).

#pragma once

#include "hardware_device.h"

#include <libretro.h>

#include <string>

namespace snaggletooth_libretro {

// Asks the frontend for the platform's hardware context (RETRO_ENVIRONMENT_SET_HW_RENDER): true
// when it will give one. The frontend calls `ready` once the context exists, and again whenever
// it makes the context anew, and `going` before it takes the context down.
bool askForHardwareContext(retro_hw_context_reset_t ready, retro_hw_context_reset_t going);

// Called from `ready`: the device made over the frontend's interface, or null with the reason. A
// device made over an earlier context is let go without a call into it, since that context may be
// gone.
HardwareDevice* openHardwareDevice(std::string& error);

// Called from `going`: the device and everything it made on the frontend's device are released.
void closeHardwareDevice();

// The device made over the current context, or null.
HardwareDevice* hardwareDevice();

// Lets go of the device without a call into the frontend's device, for retro_deinit, where the
// context may already be gone.
void abandonHardwareDevice();

// The name of the graphics API the platform's backend uses, for a message to the player:
// "Direct3D 12" on Windows, "Vulkan" on macOS and Linux.
const char* hardwareApiName() noexcept;

}  // namespace snaggletooth_libretro
