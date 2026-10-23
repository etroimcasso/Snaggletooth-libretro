// A graphics device of the frontend's, given to the core through libretro's hardware-render
// context: the machine draws each frame into a texture on it, and the frontend is handed that
// texture to show, so a frame never leaves the graphics card. One backend serves each platform's
// context, the one whose shader format the library builds there: D3d12Device (d3d12_device.h) on
// Windows, VulkanDevice (vulkan_device.h) on macOS and Linux. The session, the picture's observer
// and the context's glue (hw_render.h) hold the device through this interface and never name the
// backend.

#pragma once

#include "snaggletooth/gpu/gpu_ppu.h"

#include <cstddef>
#include <cstdint>

namespace snaggletooth_libretro {

// What the core asks of the frontend's device beyond what the machine asks of any device
// (snaggletooth::gpu::Device). Each backend also says, as static members, which libretro context
// it serves — contextType() and interfaceType() — and the API's name for a message, apiName().
class HardwareDevice : public snaggletooth::gpu::Device {
 public:
  // How an interlaced field is placed in the texture from the next frame drawn.
  virtual void place(snaggletooth::gpu::FieldPlacement placement) noexcept = 0;

  // Hands the frontend the texture's top `rows` rows, `width` texels each, as the frame to show.
  // False when the copy cannot be made.
  [[nodiscard]] virtual bool present(unsigned width, unsigned rows) = 0;
  // Hands the frontend a picture drawn on the processor, XRGB8888, row i at `first + i * pitch`.
  [[nodiscard]] virtual bool presentPixels(const std::uint32_t* first, unsigned width,
                                           unsigned rows, std::size_t pitch) = 0;

  // The texture's top `rows` rows written as XRGB8888, row i to `first + i * pitch`: the picture
  // a machine drawing elsewhere carries on from.
  [[nodiscard]] virtual bool readBack(unsigned width, unsigned rows, std::uint32_t* first,
                                      std::size_t pitch) = 0;
  // The texture's top `rows` rows set from an XRGB8888 picture, so a machine moved onto the
  // device carries on from the picture drawn so far.
  [[nodiscard]] virtual bool seed(const std::uint32_t* first, unsigned width, unsigned rows,
                                  std::size_t pitch) = 0;

  // Forgets everything made on the frontend's device without a call into it, for a context the
  // frontend may already have taken down; the destructor then calls nothing.
  virtual void abandon() noexcept = 0;
};

}  // namespace snaggletooth_libretro
