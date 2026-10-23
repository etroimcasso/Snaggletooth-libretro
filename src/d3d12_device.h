// The hardware device for libretro's Direct3D 12 context: Snaggletooth's GPU device over the
// frontend's Direct3D 12 device and queue, serving Windows.

#pragma once

#include "hardware_device.h"

#include "snaggletooth/gpu/gpu_ppu.h"
#include "snaggletooth/gpu/shader_format.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <libretro.h>
#include <libretro_d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace snaggletooth_libretro {

// The device the machine draws each frame on (SnesConfig::gpuPpu), made over the frontend's
// Direct3D 12 interface. It names the shader format the library builds for Windows, DXIL, so the
// machine hands it the library's own bytes and each pass's compute pipeline is made from them.
// The texture the device draws into holds the frontend's picture, a byte a channel: each field
// placed at the rows the interlace mode gives it, the other parity's rows kept under Weave.
// present() copies the picture into a texture the size of the picture, which the frontend is told
// about through set_texture and shows whole; presentPixels() does the same for a picture drawn on
// the processor. Every
// call arrives on the thread the frontend calls the core on. The device calls Direct3D only
// through the objects the frontend hands it, and d3d12.dll, opened at run time, for the one call
// that is not a method of them.
class D3d12Device final : public HardwareDevice {
 public:
  // The libretro context this backend serves, and its interface.
  static constexpr retro_hw_context_type contextType() noexcept { return RETRO_HW_CONTEXT_D3D12; }
  static constexpr retro_hw_render_interface_type interfaceType() noexcept {
    return RETRO_HW_RENDER_INTERFACE_D3D12;
  }
  static constexpr const char* apiName() noexcept { return "Direct3D 12"; }

  explicit D3d12Device(const retro_hw_render_interface_d3d12& frontend) : frontend_(frontend) {}
  ~D3d12Device() override;
  D3d12Device(const D3d12Device&) = delete;
  D3d12Device& operator=(const D3d12Device&) = delete;

  // Makes the texture, the scratch buffer, the root signatures and what the passes bind: false
  // with the reason when the frontend's device cannot make them.
  [[nodiscard]] bool open(std::string& error);

  // snaggletooth::gpu::Device, as the machine calls it.
  [[nodiscard]] snaggletooth::gpu::ShaderFormat formats() const override;
  bool makePipeline(snaggletooth::gpu::Pass pass, const snaggletooth::gpu::ShaderBytecode& code,
                    snaggletooth::gpu::ShaderFormat format, std::string& error) override;
  [[nodiscard]] std::span<std::uint8_t> frameBytes(std::size_t bytes) override;
  void draw(const snaggletooth::gpu::Frame& frame) override;

  // HardwareDevice, as the session calls it.
  void place(snaggletooth::gpu::FieldPlacement placement) noexcept override {
    placement_ = placement;
  }
  [[nodiscard]] bool present(unsigned width, unsigned rows) override;
  [[nodiscard]] bool presentPixels(const std::uint32_t* first, unsigned width, unsigned rows,
                                   std::size_t pitch) override;
  [[nodiscard]] bool readBack(unsigned width, unsigned rows, std::uint32_t* first,
                              std::size_t pitch) override;
  [[nodiscard]] bool seed(const std::uint32_t* first, unsigned width, unsigned rows,
                          std::size_t pitch) override;
  void abandon() noexcept override;

 private:
  // A buffer the processor writes or reads, mapped for its whole life.
  struct Buffer {
    ID3D12Resource* resource = nullptr;
    std::size_t size = 0;
    void* mapped = nullptr;
  };
  // A texture the frontend is handed, one of a ring the frontend may have in flight, the size of
  // the picture it holds (the frontend shows the whole texture), with the list that fills it and
  // the fence value its last fill reached.
  struct Shown {
    ID3D12Resource* texture = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    Buffer staging;  // the processor's picture on its way into the texture
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    std::uint64_t fenceValue = 0;
  };

  [[nodiscard]] bool makeBuffer(std::size_t size, D3D12_HEAP_TYPE heap,
                                D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                Buffer& out);
  void freeBuffer(Buffer& buffer) noexcept;
  [[nodiscard]] bool makeTexture(unsigned width, unsigned height, D3D12_RESOURCE_STATES state,
                                 D3D12_RESOURCE_FLAGS flags, ID3D12Resource*& out);
  [[nodiscard]] bool makeList(ID3D12CommandAllocator*& allocator,
                              ID3D12GraphicsCommandList*& list);
  [[nodiscard]] bool makeRootSignature(snaggletooth::gpu::Pass pass, std::string& error);
  [[nodiscard]] std::uint64_t execute(ID3D12GraphicsCommandList* list);
  void waitFor(std::uint64_t fenceValue);
  [[nodiscard]] bool runOnce();
  void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                  D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
  void record(const snaggletooth::gpu::Frame& frame, bool clear);
  void warm();
  [[nodiscard]] Shown* shownForThisFrame(unsigned width, unsigned rows);
  [[nodiscard]] bool copyRows(ID3D12GraphicsCommandList* list, ID3D12Resource* from,
                              ID3D12Resource* to, bool fromBuffer, unsigned width, unsigned rows);

  retro_hw_render_interface_d3d12 frontend_;
  bool opened_ = false;

  HMODULE d3d12_ = nullptr;  // d3d12.dll, for D3D12SerializeRootSignature

  ID3D12Fence* fence_ = nullptr;
  HANDLE fenceEvent_ = nullptr;
  std::uint64_t fenceValue_ = 0;

  ID3D12Resource* texture_ = nullptr;
  ID3D12Resource* scratch_ = nullptr;
  Buffer frame_;     // the packed frame, written by the machine and read by the passes
  Buffer uniform_;   // the row map
  Buffer upload_;    // the processor's picture on its way into the texture (seed)
  Buffer readback_;  // the texture on its way out to the processor (readBack)

  // What Pixels binds through a table: the texture's and the scratch buffer's views, in a heap
  // the shader reads, and the texture's view again in one it does not, for the clear.
  ID3D12DescriptorHeap* shaderHeap_ = nullptr;
  ID3D12DescriptorHeap* clearHeap_ = nullptr;

  ID3D12RootSignature* rootSignatures_[3] = {};
  ID3D12PipelineState* pipelines_[3] = {};

  ID3D12CommandAllocator* drawAllocator_ = nullptr;
  ID3D12GraphicsCommandList* drawList_ = nullptr;
  std::uint64_t drawFenceValue_ = 0;

  std::vector<Shown> shown_;
  std::size_t nextShown_ = 0;

  snaggletooth::gpu::FieldPlacement placement_ = snaggletooth::gpu::FieldPlacement::Weave;
  unsigned shapeWidth_ = 0;  // the picture's shape as the last frame drew it
  unsigned shapeRows_ = 0;
};

}  // namespace snaggletooth_libretro
