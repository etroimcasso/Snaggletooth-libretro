// The graphics card the console's picture is drawn on: Snaggletooth's GPU device over SDL's GPU
// API, Metal on macOS, Direct3D 12 on Windows and Vulkan on Linux, opened without a window.

#pragma once

#include "snaggletooth/gpu/gpu_ppu.h"
#include "snaggletooth/gpu/shader_format.h"

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace snaggletooth_libretro {

// The device the machine draws each frame on (SnesConfig::gpuPpu). It makes the three passes'
// compute pipelines from the shader bytes the machine selects, maps its upload buffer for the
// machine to pack a frame into, and on draw() uploads the frame and dispatches the three passes
// over its scratch buffer into its texture, each picture line on the texture row of the same
// number. readBack() then copies the frame out as the frontend's pixels. Every call arrives on
// the thread the machine is stepped on, which is the thread the frontend calls the core on.
// Once the three pipelines are made, the device runs the three passes over a frame that draws
// nothing and waits, so the GPU's first use of the texture and the pipelines, which costs
// milliseconds, is paid as the machine is built and not in the first frame.
class GpuDevice final : public snaggletooth::gpu::Device {
 public:
  GpuDevice() = default;
  ~GpuDevice() override;
  GpuDevice(const GpuDevice&) = delete;
  GpuDevice& operator=(const GpuDevice&) = delete;

  // Opens a device that takes the shader format the library was built in, with its texture and
  // scratch buffer: false with SDL's reason when none opens.
  [[nodiscard]] bool open(std::string& error);

  // snaggletooth::gpu::Device, as the machine calls it.
  [[nodiscard]] snaggletooth::gpu::ShaderFormat formats() const override;
  bool makePipeline(snaggletooth::gpu::Pass pass, const snaggletooth::gpu::ShaderBytecode& code,
                    snaggletooth::gpu::ShaderFormat format, std::string& error) override;
  [[nodiscard]] std::span<std::uint8_t> frameBytes(std::size_t bytes) override;
  void draw(const snaggletooth::gpu::Frame& frame) override;

  // The texture's top `lines` rows, `width` texels each, waited for and written as XRGB8888:
  // each channel rounded from the texture's float to its byte, red in bits 16 to 23, green in 8
  // to 15, blue in 0 to 7. Row i goes to `first + i * pitch`. False when the copy cannot be made,
  // with nothing written.
  [[nodiscard]] bool readBack(unsigned width, unsigned lines, std::uint32_t* first,
                              std::size_t pitch);

 private:
  // The three passes recorded on `command` in order over the frame last uploaded.
  void dispatch(SDL_GPUCommandBuffer* command);
  // Once the three pipelines are made: the buffers grown to a frame's usual length and the three
  // passes dispatched over a frame header of zeros, whose height of 0 draws nothing, all waited
  // for.
  void warm();

  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUComputePipeline* pipelines_[3] = {nullptr, nullptr, nullptr};  // one a pass, in Pass's order
  SDL_GPUBuffer* scratch_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  SDL_GPUTransferBuffer* upload_ = nullptr;
  SDL_GPUBuffer* buffer_ = nullptr;
  std::size_t bufferBytes_ = 0;  // the upload and storage buffers' length
  void* mapped_ = nullptr;       // the upload buffer while frameBytes() has it mapped
  SDL_GPUTransferBuffer* download_ = nullptr;
  std::size_t downloadBytes_ = 0;
  snaggletooth::gpu::Frame frame_{};  // the frame last uploaded
};

// The process's one device, opened the first time a cartridge asks to be drawn on the GPU and
// answered to every later ask, the next cartridge's included. Where none opens, the answer is
// null with SDL's reason, and the same answer is given until closeSharedGpuDevice(), so the
// device is looked for once between retro_init and retro_deinit.
GpuDevice* sharedGpuDevice(std::string& error);

// The device closed and SDL shut down with it, so the next look starts SDL afresh. Called after
// the last machine drawing on the device is gone.
void closeSharedGpuDevice();

}  // namespace snaggletooth_libretro
