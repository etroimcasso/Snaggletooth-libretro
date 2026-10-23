// The hardware device for libretro's Vulkan context: Snaggletooth's GPU device over the
// frontend's Vulkan device and queue, serving macOS, through MoltenVK, and Linux.

#pragma once

#include "hardware_device.h"

#include "snaggletooth/gpu/gpu_ppu.h"
#include "snaggletooth/gpu/shader_format.h"

#include <libretro.h>
#include <libretro_vulkan.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace snaggletooth_libretro {

// The device the machine draws each frame on (SnesConfig::gpuPpu), made over the frontend's
// Vulkan interface. It names the shader format the library builds for the platform, so the
// machine hands it the library's own bytes. On Linux those bytes are SPIR-V and each pass's
// compute pipeline is made from them. On macOS they are a Metal library, which no libretro
// context takes, so the pipeline is made from the core's own SPIR-V of the same pass: the same
// source, compiled by the library's shader system in the format Vulkan takes. The texture the
// device draws into holds the frontend's picture: each field placed at the rows the interlace
// mode gives it, the other parity's rows kept under Weave. present() copies the picture into an
// image the size of the picture, which the frontend is told about through set_image and shows
// whole; presentPixels() does the same for a picture drawn on the processor. Every call arrives
// on the thread the frontend calls the core on.
class VulkanDevice final : public HardwareDevice {
 public:
  // The libretro context this backend serves, and its interface.
  static constexpr retro_hw_context_type contextType() noexcept { return RETRO_HW_CONTEXT_VULKAN; }
  static constexpr retro_hw_render_interface_type interfaceType() noexcept {
    return RETRO_HW_RENDER_INTERFACE_VULKAN;
  }
  static constexpr const char* apiName() noexcept { return "Vulkan"; }

  explicit VulkanDevice(const retro_hw_render_interface_vulkan& frontend) : frontend_(frontend) {}
  ~VulkanDevice() override;
  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;

  // Makes the texture, the scratch buffer and what the passes bind: false with the reason when
  // the frontend's device cannot make them.
  [[nodiscard]] bool open(std::string& error);

  // snaggletooth::gpu::Device, as the machine calls it.
  [[nodiscard]] snaggletooth::gpu::ShaderFormat formats() const override;
  bool makePipeline(snaggletooth::gpu::Pass pass, const snaggletooth::gpu::ShaderBytecode& code,
                    snaggletooth::gpu::ShaderFormat format, std::string& error) override;
  [[nodiscard]] std::span<std::uint8_t> frameBytes(std::size_t bytes) override;
  void draw(const snaggletooth::gpu::Frame& frame) override;

  // HardwareDevice, as the session calls it. readBack() rounds each channel to its byte.
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
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
  };
  // An image the frontend is handed, one a frame the frontend can have in flight, the size of
  // the picture it holds: the frontend shows the whole image.
  struct Shown {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    unsigned width = 0;
    unsigned height = 0;
    retro_vulkan_image retro{};
    Buffer staging;  // the processor's picture on its way into the image
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
  };

  [[nodiscard]] bool loadFunctions();
  [[nodiscard]] bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                                Buffer& out);
  void freeBuffer(Buffer& buffer);
  [[nodiscard]] std::uint32_t memoryType(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const;
  [[nodiscard]] bool makeImage(VkFormat format, unsigned width, unsigned height,
                               VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage& image,
                               VkDeviceMemory& memory);
  void freeShownImage(Shown& shown);
  [[nodiscard]] bool makeShownImage(Shown& shown, unsigned width, unsigned height);
  [[nodiscard]] VkCommandBuffer newCommand();
  [[nodiscard]] bool submit(VkCommandBuffer command, VkFence fence);
  void waitFor(VkFence fence, bool& submitted);
  [[nodiscard]] bool runOnce(VkCommandBuffer command);
  void bindFrame();
  void record(VkCommandBuffer command, const snaggletooth::gpu::Frame& frame, bool clear);
  void warm();
  [[nodiscard]] Shown* shownForThisFrame(unsigned width, unsigned rows);
  void handOver(Shown& shown);

  retro_hw_render_interface_vulkan frontend_;

  // The Vulkan functions the device calls, fetched from the frontend's loader.
  struct Functions;
  Functions* vk_ = nullptr;

  VkPhysicalDeviceMemoryProperties memoryProperties_{};
  VkCommandPool pool_ = VK_NULL_HANDLE;

  VkImage texture_ = VK_NULL_HANDLE;
  VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
  VkImageView textureView_ = VK_NULL_HANDLE;
  Buffer scratch_;
  Buffer frame_;     // the packed frame, written by the machine and read by the passes
  Buffer uniform_;   // the row map
  Buffer transfer_;  // the texture on its way in from, or out to, the processor

  VkDescriptorSetLayout layouts_[4] = {};  // the frame; Sprites' and Tiles' writes; Pixels'; the uniform
  VkPipelineLayout pipelineLayouts_[3] = {};
  VkPipeline pipelines_[3] = {};
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSet frameSet_ = VK_NULL_HANDLE;
  VkDescriptorSet scratchSet_ = VK_NULL_HANDLE;
  VkDescriptorSet pixelsSet_ = VK_NULL_HANDLE;
  VkDescriptorSet uniformSet_ = VK_NULL_HANDLE;

  VkCommandBuffer drawCommand_ = VK_NULL_HANDLE;
  VkFence drawFence_ = VK_NULL_HANDLE;
  bool drawSubmitted_ = false;

  std::vector<Shown> shown_;

  snaggletooth::gpu::FieldPlacement placement_ = snaggletooth::gpu::FieldPlacement::Weave;
  unsigned shapeWidth_ = 0;  // the picture's shape as the last frame drew it
  unsigned shapeRows_ = 0;
};

}  // namespace snaggletooth_libretro
