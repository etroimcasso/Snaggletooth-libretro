#include "hw_render.h"

#include "frontend.h"

#include <memory>
#include <utility>

// The backend for the platform's context, the one whose shader format the library builds there:
// the Direct3D 12 device on Windows, the Vulkan device on macOS and Linux. Each platform compiles
// its own backend alone (CMakeLists.txt).
#ifdef _WIN32
#include "d3d12_device.h"
#else
#include "vulkan_device.h"
#endif

namespace snaggletooth_libretro {

namespace {

#ifdef _WIN32
using Backend = D3d12Device;
using Interface = retro_hw_render_interface_d3d12;
#else
using Backend = VulkanDevice;
using Interface = retro_hw_render_interface_vulkan;
#endif

// The frontend keeps a pointer to the callback for the life of the context.
retro_hw_render_callback callback{};
std::unique_ptr<HardwareDevice> device;

}  // namespace

bool askForHardwareContext(retro_hw_context_reset_t ready, retro_hw_context_reset_t going) {
  callback = retro_hw_render_callback{};
  callback.context_type = Backend::contextType();
  callback.context_reset = ready;
  callback.context_destroy = going;
  return environment != nullptr && environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &callback);
}

HardwareDevice* openHardwareDevice(std::string& error) {
  abandonHardwareDevice();
  const retro_hw_render_interface* face = nullptr;
  if (environment == nullptr || !environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &face) ||
      face == nullptr || face->interface_type != Backend::interfaceType()) {
    error = std::string("the frontend gave no ") + Backend::apiName() + " interface";
    return nullptr;
  }
  auto made = std::make_unique<Backend>(*reinterpret_cast<const Interface*>(face));
  if (!made->open(error)) return nullptr;
  device = std::move(made);
  return device.get();
}

void closeHardwareDevice() { device.reset(); }

HardwareDevice* hardwareDevice() { return device.get(); }

void abandonHardwareDevice() {
  if (device) device->abandon();
  device.reset();
}

const char* hardwareApiName() noexcept { return Backend::apiName(); }

}  // namespace snaggletooth_libretro
