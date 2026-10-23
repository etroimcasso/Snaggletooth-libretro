#include "vulkan_device.h"

// The core's own SPIR-V of the three passes, where the build compiles it (CMakeLists.txt).
#if SNAGGLETOOTH_LIBRETRO_OWN_SPIRV
#include "snaggletooth/gpu/shaders/ppu_pixels.vulkan.comp.h"
#include "snaggletooth/gpu/shaders/ppu_sprites.vulkan.comp.h"
#include "snaggletooth/gpu/shaders/ppu_tiles.vulkan.comp.h"
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace snaggletooth_libretro {

namespace {

namespace gpu = snaggletooth::gpu;

// The texture holds each channel as the shader writes it, a float, the byte over 255.
constexpr VkFormat kTextureFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr std::size_t kTexelBytes = 16u;
// The images the frontend is handed: a byte a channel, blue first, as an XRGB8888 word lies in
// memory, so a picture drawn on the processor is copied in as it is.
constexpr VkFormat kShownFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr std::size_t kShownBytes =
    static_cast<std::size_t>(gpu::kTextureWidth) * gpu::kTextureHeight * 4u;
// What warm() draws: a frame header of zeros, whose height of 0 leaves every thread of each pass
// drawing nothing, in buffers of a length a frame with no VRAM write fits.
constexpr std::size_t kWarmBytes = 131072u;
constexpr std::size_t kWarmHeaderBytes = 128u;

// The core's own SPIR-V of a pass, compiled from the library's shader source by the library's
// shader system where the library's format is not SPIR-V (macOS); empty on a build that compiles
// none (Linux, where the library's bytes are SPIR-V already).
gpu::ShaderBytecode ownSpirv(gpu::Pass pass) noexcept {
#if SNAGGLETOOTH_LIBRETRO_OWN_SPIRV
  switch (pass) {
    case gpu::Pass::Sprites: return snaggletooth::shaders::ppu_sprites_vulkan_comp.spirv;
    case gpu::Pass::Tiles: return snaggletooth::shaders::ppu_tiles_vulkan_comp.spirv;
    case gpu::Pass::Pixels: break;
  }
  return snaggletooth::shaders::ppu_pixels_vulkan_comp.spirv;
#else
  (void)pass;
  return gpu::ShaderBytecode{};
#endif
}

// The formats the library carries the shader in: the platform's one, the slot the build filled.
gpu::ShaderFormat builtFormats() noexcept {
  const gpu::ShaderVariants& shader = gpu::ppuShader(gpu::Pass::Pixels);
  gpu::ShaderFormat formats = gpu::ShaderFormat::None;
  if (shader.spirv.data != nullptr) formats = formats | gpu::ShaderFormat::Spirv;
  if (shader.dxil.data != nullptr) formats = formats | gpu::ShaderFormat::Dxil;
  if (shader.metallib.data != nullptr) formats = formats | gpu::ShaderFormat::Metallib;
  return formats;
}

[[nodiscard]] std::uint32_t byteOf(float channel) noexcept {
  return static_cast<std::uint32_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

#define SNAGGLETOOTH_LIBRETRO_VULKAN_FUNCTIONS(X) \
  X(AllocateCommandBuffers)                       \
  X(AllocateDescriptorSets)                       \
  X(AllocateMemory)                               \
  X(BeginCommandBuffer)                           \
  X(BindBufferMemory)                             \
  X(BindImageMemory)                              \
  X(CmdBindDescriptorSets)                        \
  X(CmdBindPipeline)                              \
  X(CmdBlitImage)                                 \
  X(CmdClearColorImage)                           \
  X(CmdCopyBufferToImage)                         \
  X(CmdCopyImageToBuffer)                         \
  X(CmdDispatch)                                  \
  X(CmdPipelineBarrier)                           \
  X(CreateBuffer)                                 \
  X(CreateCommandPool)                            \
  X(CreateComputePipelines)                       \
  X(CreateDescriptorPool)                         \
  X(CreateDescriptorSetLayout)                    \
  X(CreateFence)                                  \
  X(CreateImage)                                  \
  X(CreateImageView)                              \
  X(CreatePipelineLayout)                         \
  X(CreateShaderModule)                           \
  X(DestroyBuffer)                                \
  X(DestroyCommandPool)                           \
  X(DestroyDescriptorPool)                        \
  X(DestroyDescriptorSetLayout)                   \
  X(DestroyFence)                                 \
  X(DestroyImage)                                 \
  X(DestroyImageView)                             \
  X(DestroyPipeline)                              \
  X(DestroyPipelineLayout)                        \
  X(DestroyShaderModule)                          \
  X(DeviceWaitIdle)                               \
  X(EndCommandBuffer)                             \
  X(FreeCommandBuffers)                           \
  X(FreeMemory)                                   \
  X(GetBufferMemoryRequirements)                  \
  X(GetImageMemoryRequirements)                   \
  X(MapMemory)                                    \
  X(QueueSubmit)                                  \
  X(ResetCommandBuffer)                           \
  X(ResetFences)                                  \
  X(UpdateDescriptorSets)                         \
  X(WaitForFences)

struct VulkanDevice::Functions {
#define SNAGGLETOOTH_LIBRETRO_DECLARE(name) PFN_vk##name name = nullptr;
  SNAGGLETOOTH_LIBRETRO_VULKAN_FUNCTIONS(SNAGGLETOOTH_LIBRETRO_DECLARE)
#undef SNAGGLETOOTH_LIBRETRO_DECLARE
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
};

namespace {

void memoryBarrier(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer command,
                   VkPipelineStageFlags from, VkAccessFlags written, VkPipelineStageFlags to,
                   VkAccessFlags read) {
  const VkMemoryBarrier memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .pNext = nullptr,
                               .srcAccessMask = written,
                               .dstAccessMask = read};
  barrier(command, from, to, 0u, 1u, &memory, 0u, nullptr, 0u, nullptr);
}

void imageBarrier(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer command, VkImage image,
                  VkImageLayout fromLayout, VkImageLayout toLayout, VkPipelineStageFlags from,
                  VkAccessFlags written, VkPipelineStageFlags to, VkAccessFlags read) {
  const VkImageMemoryBarrier transition{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .pNext = nullptr,
      .srcAccessMask = written,
      .dstAccessMask = read,
      .oldLayout = fromLayout,
      .newLayout = toLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = 0u,
                           .levelCount = 1u,
                           .baseArrayLayer = 0u,
                           .layerCount = 1u}};
  barrier(command, from, to, 0u, 0u, nullptr, 0u, nullptr, 1u, &transition);
}

constexpr VkImageSubresourceLayers kColorLayer{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                               .mipLevel = 0u,
                                               .baseArrayLayer = 0u,
                                               .layerCount = 1u};

}  // namespace

VulkanDevice::~VulkanDevice() {
  if (vk_ == nullptr) return;
  VkDevice device = frontend_.device;
  frontend_.lock_queue(frontend_.handle);
  vk_->DeviceWaitIdle(device);
  frontend_.unlock_queue(frontend_.handle);
  for (Shown& shown : shown_) {
    freeBuffer(shown.staging);
    if (shown.fence != VK_NULL_HANDLE) vk_->DestroyFence(device, shown.fence, nullptr);
    freeShownImage(shown);
  }
  if (drawFence_ != VK_NULL_HANDLE) vk_->DestroyFence(device, drawFence_, nullptr);
  if (pool_ != VK_NULL_HANDLE) vk_->DestroyCommandPool(device, pool_, nullptr);
  for (VkPipeline pipeline : pipelines_) {
    if (pipeline != VK_NULL_HANDLE) vk_->DestroyPipeline(device, pipeline, nullptr);
  }
  for (VkPipelineLayout layout : pipelineLayouts_) {
    if (layout != VK_NULL_HANDLE) vk_->DestroyPipelineLayout(device, layout, nullptr);
  }
  if (descriptorPool_ != VK_NULL_HANDLE) vk_->DestroyDescriptorPool(device, descriptorPool_, nullptr);
  for (VkDescriptorSetLayout layout : layouts_) {
    if (layout != VK_NULL_HANDLE) vk_->DestroyDescriptorSetLayout(device, layout, nullptr);
  }
  freeBuffer(transfer_);
  freeBuffer(uniform_);
  freeBuffer(frame_);
  freeBuffer(scratch_);
  if (textureView_ != VK_NULL_HANDLE) vk_->DestroyImageView(device, textureView_, nullptr);
  if (texture_ != VK_NULL_HANDLE) vk_->DestroyImage(device, texture_, nullptr);
  if (textureMemory_ != VK_NULL_HANDLE) vk_->FreeMemory(device, textureMemory_, nullptr);
  delete vk_;
}

void VulkanDevice::abandon() noexcept {
  delete vk_;
  vk_ = nullptr;
}

bool VulkanDevice::loadFunctions() {
  vk_ = new Functions{};
  bool all = true;
#define SNAGGLETOOTH_LIBRETRO_LOAD(name)                                                      \
  vk_->name = reinterpret_cast<PFN_vk##name>(                                                \
      frontend_.get_device_proc_addr(frontend_.device, "vk" #name));                         \
  all = all && vk_->name != nullptr;
  SNAGGLETOOTH_LIBRETRO_VULKAN_FUNCTIONS(SNAGGLETOOTH_LIBRETRO_LOAD)
#undef SNAGGLETOOTH_LIBRETRO_LOAD
  vk_->GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      frontend_.get_instance_proc_addr(frontend_.instance, "vkGetPhysicalDeviceMemoryProperties"));
  vk_->GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      frontend_.get_instance_proc_addr(frontend_.instance, "vkGetPhysicalDeviceFormatProperties"));
  return all && vk_->GetPhysicalDeviceMemoryProperties != nullptr &&
         vk_->GetPhysicalDeviceFormatProperties != nullptr;
}

std::uint32_t VulkanDevice::memoryType(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
  for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
    if ((allowed & (1u << i)) != 0u &&
        (memoryProperties_.memoryTypes[i].propertyFlags & wanted) == wanted) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool VulkanDevice::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                              Buffer& out) {
  VkDevice device = frontend_.device;
  const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0u,
                                .size = size,
                                .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .queueFamilyIndexCount = 0u,
                                .pQueueFamilyIndices = nullptr};
  if (vk_->CreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements needs{};
  vk_->GetBufferMemoryRequirements(device, out.buffer, &needs);
  const VkMemoryPropertyFlags wanted =
      hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                  : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  std::uint32_t type = memoryType(needs.memoryTypeBits, wanted);
  if (type == UINT32_MAX && !hostVisible) type = memoryType(needs.memoryTypeBits, 0u);
  if (type == UINT32_MAX) return false;
  const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = nullptr,
                                      .allocationSize = needs.size,
                                      .memoryTypeIndex = type};
  if (vk_->AllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
  if (vk_->BindBufferMemory(device, out.buffer, out.memory, 0u) != VK_SUCCESS) return false;
  if (hostVisible && vk_->MapMemory(device, out.memory, 0u, size, 0u, &out.mapped) != VK_SUCCESS) {
    return false;
  }
  out.size = size;
  return true;
}

void VulkanDevice::freeBuffer(Buffer& buffer) {
  VkDevice device = frontend_.device;
  // Freeing the memory unmaps it.
  if (buffer.buffer != VK_NULL_HANDLE) vk_->DestroyBuffer(device, buffer.buffer, nullptr);
  if (buffer.memory != VK_NULL_HANDLE) vk_->FreeMemory(device, buffer.memory, nullptr);
  buffer = Buffer{};
}

bool VulkanDevice::makeImage(VkFormat format, unsigned width, unsigned height,
                             VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage& image,
                             VkDeviceMemory& memory) {
  VkDevice device = frontend_.device;
  const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .pNext = nullptr,
                               .flags = flags,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = format,
                               .extent = {width, height, 1u},
                               .mipLevels = 1u,
                               .arrayLayers = 1u,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .queueFamilyIndexCount = 0u,
                               .pQueueFamilyIndices = nullptr,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  if (vk_->CreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
  VkMemoryRequirements needs{};
  vk_->GetImageMemoryRequirements(device, image, &needs);
  std::uint32_t type = memoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) type = memoryType(needs.memoryTypeBits, 0u);
  if (type == UINT32_MAX) return false;
  const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = nullptr,
                                      .allocationSize = needs.size,
                                      .memoryTypeIndex = type};
  if (vk_->AllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
  return vk_->BindImageMemory(device, image, memory, 0u) == VK_SUCCESS;
}

void VulkanDevice::freeShownImage(Shown& shown) {
  VkDevice device = frontend_.device;
  if (shown.retro.image_view != VK_NULL_HANDLE) {
    vk_->DestroyImageView(device, shown.retro.image_view, nullptr);
  }
  if (shown.image != VK_NULL_HANDLE) vk_->DestroyImage(device, shown.image, nullptr);
  if (shown.memory != VK_NULL_HANDLE) vk_->FreeMemory(device, shown.memory, nullptr);
  shown.retro.image_view = VK_NULL_HANDLE;
  shown.image = VK_NULL_HANDLE;
  shown.memory = VK_NULL_HANDLE;
  shown.width = 0u;
  shown.height = 0u;
}

bool VulkanDevice::makeShownImage(Shown& shown, unsigned width, unsigned height) {
  freeShownImage(shown);
  if (!makeImage(kShownFormat, width, height,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, shown.image, shown.memory)) {
    return false;
  }
  shown.retro.create_info = VkImageViewCreateInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0u,
      .image = shown.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = kShownFormat,
      .components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                     VK_COMPONENT_SWIZZLE_A},
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
  shown.retro.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (vk_->CreateImageView(frontend_.device, &shown.retro.create_info, nullptr,
                           &shown.retro.image_view) != VK_SUCCESS) {
    return false;
  }
  shown.width = width;
  shown.height = height;
  return true;
}

VkCommandBuffer VulkanDevice::newCommand() {
  const VkCommandBufferAllocateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .pNext = nullptr,
                                         .commandPool = pool_,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1u};
  VkCommandBuffer command = VK_NULL_HANDLE;
  if (vk_->AllocateCommandBuffers(frontend_.device, &info, &command) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return command;
}

bool VulkanDevice::submit(VkCommandBuffer command, VkFence fence) {
  const VkSubmitInfo info{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .pNext = nullptr,
                          .waitSemaphoreCount = 0u,
                          .pWaitSemaphores = nullptr,
                          .pWaitDstStageMask = nullptr,
                          .commandBufferCount = 1u,
                          .pCommandBuffers = &command,
                          .signalSemaphoreCount = 0u,
                          .pSignalSemaphores = nullptr};
  // The queue is the frontend's, so every submission holds the frontend's lock on it.
  frontend_.lock_queue(frontend_.handle);
  const VkResult result = vk_->QueueSubmit(frontend_.queue, 1u, &info, fence);
  frontend_.unlock_queue(frontend_.handle);
  return result == VK_SUCCESS;
}

void VulkanDevice::waitFor(VkFence fence, bool& submitted) {
  if (!submitted) return;
  vk_->WaitForFences(frontend_.device, 1u, &fence, VK_TRUE, UINT64_MAX);
  vk_->ResetFences(frontend_.device, 1u, &fence);
  submitted = false;
}

bool VulkanDevice::runOnce(VkCommandBuffer command) {
  waitFor(drawFence_, drawSubmitted_);
  const bool ran = vk_->EndCommandBuffer(command) == VK_SUCCESS && submit(command, drawFence_);
  drawSubmitted_ = ran;
  waitFor(drawFence_, drawSubmitted_);
  vk_->FreeCommandBuffers(frontend_.device, pool_, 1u, &command);
  return ran;
}

bool VulkanDevice::open(std::string& error) {
  if (!loadFunctions()) {
    error = "the frontend's Vulkan device lacks a function the core calls";
    return false;
  }
  VkDevice device = frontend_.device;
  vk_->GetPhysicalDeviceMemoryProperties(frontend_.gpu, &memoryProperties_);

  VkFormatProperties texture{};
  vk_->GetPhysicalDeviceFormatProperties(frontend_.gpu, kTextureFormat, &texture);
  VkFormatProperties shown{};
  vk_->GetPhysicalDeviceFormatProperties(frontend_.gpu, kShownFormat, &shown);
  if ((texture.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u ||
      (texture.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0u ||
      (shown.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0u) {
    error = "the frontend's Vulkan device cannot write and copy the picture's texture";
    return false;
  }

  const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = frontend_.queue_index};
  const VkFenceCreateInfo fenceInfo{
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0u};
  if (vk_->CreateCommandPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS ||
      vk_->CreateFence(device, &fenceInfo, nullptr, &drawFence_) != VK_SUCCESS) {
    error = "the frontend's Vulkan device made no command pool";
    return false;
  }
  drawCommand_ = newCommand();

  if (!makeImage(kTextureFormat, gpu::kTextureWidth, gpu::kTextureHeight,
                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 0u, texture_, textureMemory_)) {
    error = "the frontend's Vulkan device made no texture for the picture";
    return false;
  }
  const VkImageViewCreateInfo viewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0u,
      .image = texture_,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = kTextureFormat,
      .components = {},
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
  if (vk_->CreateImageView(device, &viewInfo, nullptr, &textureView_) != VK_SUCCESS ||
      drawCommand_ == VK_NULL_HANDLE ||
      !makeBuffer(gpu::kScratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch_) ||
      !makeBuffer(gpu::kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, uniform_)) {
    error = "the frontend's Vulkan device made no buffers for the picture";
    return false;
  }

  // What each pass binds, set by set as the shaders declare them: the frame in set 0; in set 1
  // the scratch buffer, after the texture for Pixels; Pixels' uniform in set 2.
  const auto layoutOf = [&](std::initializer_list<VkDescriptorType> types, VkDescriptorSetLayout& out) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const VkDescriptorType type : types) {
      bindings.push_back(VkDescriptorSetLayoutBinding{
          .binding = static_cast<std::uint32_t>(bindings.size()),
          .descriptorType = type,
          .descriptorCount = 1u,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = nullptr});
    }
    const VkDescriptorSetLayoutCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0u,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data()};
    return vk_->CreateDescriptorSetLayout(device, &info, nullptr, &out) == VK_SUCCESS;
  };
  if (!layoutOf({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, layouts_[0]) ||
      !layoutOf({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, layouts_[1]) ||
      !layoutOf({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, layouts_[2]) ||
      !layoutOf({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, layouts_[3])) {
    error = "the frontend's Vulkan device made no descriptor layouts";
    return false;
  }
  const VkDescriptorSetLayout spritesAndTiles[] = {layouts_[0], layouts_[1]};
  const VkDescriptorSetLayout pixels[] = {layouts_[0], layouts_[2], layouts_[3]};
  for (unsigned pass = 0; pass < 3u; ++pass) {
    const bool isPixels = pass == static_cast<unsigned>(gpu::Pass::Pixels);
    const VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .pNext = nullptr,
                                          .flags = 0u,
                                          .setLayoutCount = isPixels ? 3u : 2u,
                                          .pSetLayouts = isPixels ? pixels : spritesAndTiles,
                                          .pushConstantRangeCount = 0u,
                                          .pPushConstantRanges = nullptr};
    if (vk_->CreatePipelineLayout(device, &info, nullptr, &pipelineLayouts_[pass]) != VK_SUCCESS) {
      error = "the frontend's Vulkan device made no pipeline layout";
      return false;
    }
  }

  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3u},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u}};
  const VkDescriptorPoolCreateInfo poolSizes{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .pNext = nullptr,
                                             .flags = 0u,
                                             .maxSets = 4u,
                                             .poolSizeCount = 3u,
                                             .pPoolSizes = sizes};
  if (vk_->CreateDescriptorPool(device, &poolSizes, nullptr, &descriptorPool_) != VK_SUCCESS) {
    error = "the frontend's Vulkan device made no descriptor pool";
    return false;
  }
  VkDescriptorSet sets[4] = {};
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .pNext = nullptr,
                                             .descriptorPool = descriptorPool_,
                                             .descriptorSetCount = 4u,
                                             .pSetLayouts = layouts_};
  if (vk_->AllocateDescriptorSets(device, &allocate, sets) != VK_SUCCESS) {
    error = "the frontend's Vulkan device made no descriptor sets";
    return false;
  }
  frameSet_ = sets[0];
  scratchSet_ = sets[1];
  pixelsSet_ = sets[2];
  uniformSet_ = sets[3];
  const VkDescriptorBufferInfo scratch{scratch_.buffer, 0u, VK_WHOLE_SIZE};
  const VkDescriptorBufferInfo uniform{uniform_.buffer, 0u, VK_WHOLE_SIZE};
  const VkDescriptorImageInfo picture{VK_NULL_HANDLE, textureView_, VK_IMAGE_LAYOUT_GENERAL};
  const auto write = [](VkDescriptorSet set, std::uint32_t binding, VkDescriptorType type,
                        const VkDescriptorBufferInfo* buffer, const VkDescriptorImageInfo* image) {
    return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .pNext = nullptr,
                                .dstSet = set,
                                .dstBinding = binding,
                                .dstArrayElement = 0u,
                                .descriptorCount = 1u,
                                .descriptorType = type,
                                .pImageInfo = image,
                                .pBufferInfo = buffer,
                                .pTexelBufferView = nullptr};
  };
  const VkWriteDescriptorSet writes[] = {
      write(scratchSet_, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &scratch, nullptr),
      write(pixelsSet_, 0u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &picture),
      write(pixelsSet_, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &scratch, nullptr),
      write(uniformSet_, 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniform, nullptr)};
  vk_->UpdateDescriptorSets(device, 4u, writes, 0u, nullptr);

  // The texture starts black and stays in the general layout, where the passes write it and the
  // copies read it.
  VkCommandBuffer command = newCommand();
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (command == VK_NULL_HANDLE || vk_->BeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    error = "the frontend's Vulkan device made no command buffer";
    return false;
  }
  imageBarrier(vk_->CmdPipelineBarrier, command, texture_, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0u,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  const VkClearColorValue black{};
  const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  vk_->CmdClearColorImage(command, texture_, VK_IMAGE_LAYOUT_GENERAL, &black, 1u, &whole);
  if (!runOnce(command)) {
    error = "the frontend's Vulkan queue did not run the core's work";
    return false;
  }
  return true;
}

gpu::ShaderFormat VulkanDevice::formats() const {
  // The library's own format, so the machine hands over the bytes the library carries;
  // makePipeline() takes them as they are when they are SPIR-V.
  return vk_ == nullptr ? gpu::ShaderFormat::None : builtFormats();
}

bool VulkanDevice::makePipeline(gpu::Pass pass, const gpu::ShaderBytecode& code,
                                gpu::ShaderFormat format, std::string& error) {
  // The bytes the machine hands over where they are SPIR-V (Linux); the core's own SPIR-V of
  // the pass where the library's format is another (macOS, where it is a Metal library).
  const gpu::ShaderBytecode bytes = format == gpu::ShaderFormat::Spirv ? code : ownSpirv(pass);
  if (bytes.data == nullptr) {
    error = "this build of the core has no SPIR-V of the passes";
    return false;
  }
  VkDevice device = frontend_.device;
  const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0u,
                                            .codeSize = bytes.size,
                                            .pCode = reinterpret_cast<const std::uint32_t*>(bytes.data)};
  VkShaderModule module = VK_NULL_HANDLE;
  if (vk_->CreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
    error = "the frontend's Vulkan device refused the pass's SPIR-V";
    return false;
  }
  const unsigned index = static_cast<unsigned>(pass);
  const VkComputePipelineCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0u,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0u,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = bytes.entrypoint,
                .pSpecializationInfo = nullptr},
      .layout = pipelineLayouts_[index],
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = -1};
  const VkResult made =
      vk_->CreateComputePipelines(device, VK_NULL_HANDLE, 1u, &info, nullptr, &pipelines_[index]);
  vk_->DestroyShaderModule(device, module, nullptr);
  if (made != VK_SUCCESS) {
    pipelines_[index] = VK_NULL_HANDLE;
    error = "the frontend's Vulkan device made no compute pipeline";
    return false;
  }
  if (pipelines_[0] != VK_NULL_HANDLE && pipelines_[1] != VK_NULL_HANDLE &&
      pipelines_[2] != VK_NULL_HANDLE) {
    warm();
  }
  return true;
}

void VulkanDevice::bindFrame() {
  const VkDescriptorBufferInfo frame{frame_.buffer, 0u, VK_WHOLE_SIZE};
  const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .pNext = nullptr,
                                   .dstSet = frameSet_,
                                   .dstBinding = 0u,
                                   .dstArrayElement = 0u,
                                   .descriptorCount = 1u,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   .pImageInfo = nullptr,
                                   .pBufferInfo = &frame,
                                   .pTexelBufferView = nullptr};
  vk_->UpdateDescriptorSets(frontend_.device, 1u, &write, 0u, nullptr);
}

std::span<std::uint8_t> VulkanDevice::frameBytes(std::size_t bytes) {
  // The passes of the frame before read the buffer, so it is written only once they are done.
  waitFor(drawFence_, drawSubmitted_);
  // The buffer grows to the largest frame and never shrinks.
  if (bytes > frame_.size) {
    freeBuffer(frame_);
    if (!makeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, frame_)) {
      freeBuffer(frame_);
      return {};
    }
    bindFrame();
  }
  return std::span<std::uint8_t>(static_cast<std::uint8_t*>(frame_.mapped), frame_.size);
}

void VulkanDevice::record(VkCommandBuffer command, const gpu::Frame& frame, bool clear) {
  const PFN_vkCmdPipelineBarrier barrier = vk_->CmdPipelineBarrier;
  // The frame before's passes and copies are done with the scratch buffer and the texture.
  memoryBarrier(barrier, command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  if (clear) {
    const VkClearColorValue black{};
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vk_->CmdClearColorImage(command, texture_, VK_IMAGE_LAYOUT_GENERAL, &black, 1u, &whole);
    memoryBarrier(barrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
  for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
    const unsigned index = static_cast<unsigned>(pass);
    const bool pixels = pass == gpu::Pass::Pixels;
    const VkDescriptorSet sets[] = {frameSet_, pixels ? pixelsSet_ : scratchSet_, uniformSet_};
    vk_->CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[index]);
    vk_->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayouts_[index],
                               0u, pixels ? 3u : 2u, sets, 0u, nullptr);
    const gpu::Groups groups = gpu::groupsFor(pass, frame);
    vk_->CmdDispatch(command, groups.x, groups.y, groups.z);
    memoryBarrier(barrier, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT);
  }
}

void VulkanDevice::draw(const gpu::Frame& frame) {
  if (frame_.mapped == nullptr || pipelines_[0] == VK_NULL_HANDLE ||
      pipelines_[1] == VK_NULL_HANDLE || pipelines_[2] == VK_NULL_HANDLE) {
    return;
  }
  waitFor(drawFence_, drawSubmitted_);
  // Each field at the rows the interlace mode places it at. A woven or doubled picture of a new
  // shape starts black, as the processor's picture does (Picture::place).
  const gpu::RowMap rows = gpu::rowMapFor(placement_, frame.interlaced, frame.field);
  const gpu::Uniform uniform = gpu::uniformFor(rows);
  std::memcpy(uniform_.mapped, &uniform, sizeof uniform);
  const unsigned shapeRows = gpu::rowsUnder(rows, frame.height);
  const bool clear = rows.step == 2u && (frame.width != shapeWidth_ || shapeRows != shapeRows_);
  shapeWidth_ = frame.width;
  shapeRows_ = shapeRows;

  vk_->ResetCommandBuffer(drawCommand_, 0u);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (vk_->BeginCommandBuffer(drawCommand_, &begin) != VK_SUCCESS) return;
  record(drawCommand_, frame, clear);
  if (vk_->EndCommandBuffer(drawCommand_) != VK_SUCCESS) return;
  drawSubmitted_ = submit(drawCommand_, drawFence_);
}

void VulkanDevice::warm() {
  const std::span<std::uint8_t> bytes = frameBytes(kWarmBytes);
  if (bytes.empty()) return;
  std::memset(bytes.data(), 0, kWarmHeaderBytes);
  draw(gpu::Frame{.width = 256u, .height = 1u, .firstLine = 1u, .field = 0u,
                  .interlaced = false, .bytes = kWarmHeaderBytes});
  waitFor(drawFence_, drawSubmitted_);
  shapeWidth_ = 0u;
  shapeRows_ = 0u;
}

VulkanDevice::Shown* VulkanDevice::shownForThisFrame(unsigned width, unsigned rows) {
  const std::uint32_t mask = frontend_.get_sync_index_mask(frontend_.handle);
  const std::uint32_t count = mask == 0u ? 1u : static_cast<std::uint32_t>(std::bit_width(mask));
  VkDevice device = frontend_.device;
  while (shown_.size() < count) {
    Shown shown;
    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0u};
    shown.command = newCommand();
    if (!makeBuffer(kShownBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, shown.staging) ||
        vk_->CreateFence(device, &fenceInfo, nullptr, &shown.fence) != VK_SUCCESS ||
        shown.command == VK_NULL_HANDLE) {
      shown_.push_back(shown);
      return nullptr;
    }
    shown_.push_back(shown);
  }
  const std::uint32_t index = frontend_.get_sync_index(frontend_.handle);
  if (index >= shown_.size()) return nullptr;
  // The frontend is done with this frame's image, and the core's last copy into it, once the
  // frontend's work for the sync index is.
  frontend_.wait_sync_index(frontend_.handle);
  Shown& shown = shown_[index];
  waitFor(shown.fence, shown.submitted);
  if (shown.staging.mapped == nullptr || shown.fence == VK_NULL_HANDLE) return nullptr;
  // The image is the picture's size, so the frontend shows the picture and nothing beside it;
  // a picture of a new shape gets a new image.
  if ((shown.width != width || shown.height != rows) && !makeShownImage(shown, width, rows)) {
    return nullptr;
  }
  return &shown;
}

void VulkanDevice::handOver(Shown& shown) {
  frontend_.set_image(frontend_.handle, &shown.retro, 0u, nullptr, VK_QUEUE_FAMILY_IGNORED);
}

bool VulkanDevice::present(unsigned width, unsigned rows) {
  Shown* shown = shownForThisFrame(width, rows);
  if (shown == nullptr) return false;
  const PFN_vkCmdPipelineBarrier barrier = vk_->CmdPipelineBarrier;
  VkCommandBuffer command = shown->command;
  vk_->ResetCommandBuffer(command, 0u);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (vk_->BeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
  memoryBarrier(barrier, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
  imageBarrier(barrier, command, shown->image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0u,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  const VkOffset3D corner{static_cast<std::int32_t>(width), static_cast<std::int32_t>(rows), 1};
  const VkImageBlit region{.srcSubresource = kColorLayer,
                           .srcOffsets = {{0, 0, 0}, corner},
                           .dstSubresource = kColorLayer,
                           .dstOffsets = {{0, 0, 0}, corner}};
  vk_->CmdBlitImage(command, texture_, VK_IMAGE_LAYOUT_GENERAL, shown->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region, VK_FILTER_NEAREST);
  imageBarrier(barrier, command, shown->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  if (vk_->EndCommandBuffer(command) != VK_SUCCESS) return false;
  shown->submitted = submit(command, shown->fence);
  if (!shown->submitted) return false;
  handOver(*shown);
  return true;
}

bool VulkanDevice::presentPixels(const std::uint32_t* first, unsigned width, unsigned rows,
                                 std::size_t pitch) {
  Shown* shown = shownForThisFrame(width, rows);
  if (shown == nullptr) return false;
  // Each XRGB8888 word lies in memory as blue, green, red and a spare byte, the image's order;
  // the spare byte is made opaque.
  auto* to = static_cast<std::uint32_t*>(shown->staging.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    const std::uint32_t* from = first + static_cast<std::size_t>(row) * pitch;
    std::uint32_t* line = to + static_cast<std::size_t>(row) * width;
    for (unsigned x = 0; x < width; ++x) line[x] = from[x] | 0xFF000000u;
  }
  const PFN_vkCmdPipelineBarrier barrier = vk_->CmdPipelineBarrier;
  VkCommandBuffer command = shown->command;
  vk_->ResetCommandBuffer(command, 0u);
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (vk_->BeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
  imageBarrier(barrier, command, shown->image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0u,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  const VkBufferImageCopy region{.bufferOffset = 0u,
                                 .bufferRowLength = width,
                                 .bufferImageHeight = rows,
                                 .imageSubresource = kColorLayer,
                                 .imageOffset = {0, 0, 0},
                                 .imageExtent = {width, rows, 1u}};
  vk_->CmdCopyBufferToImage(command, shown->staging.buffer, shown->image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
  imageBarrier(barrier, command, shown->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  if (vk_->EndCommandBuffer(command) != VK_SUCCESS) return false;
  shown->submitted = submit(command, shown->fence);
  if (!shown->submitted) return false;
  handOver(*shown);
  return true;
}

bool VulkanDevice::readBack(unsigned width, unsigned rows, std::uint32_t* first,
                            std::size_t pitch) {
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * rows * kTexelBytes;
  if (bytes > transfer_.size) {
    freeBuffer(transfer_);
    if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true, transfer_)) {
      freeBuffer(transfer_);
      return false;
    }
  }
  VkCommandBuffer command = newCommand();
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (command == VK_NULL_HANDLE || vk_->BeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    return false;
  }
  memoryBarrier(vk_->CmdPipelineBarrier, command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
  const VkBufferImageCopy region{.bufferOffset = 0u,
                                 .bufferRowLength = width,
                                 .bufferImageHeight = rows,
                                 .imageSubresource = kColorLayer,
                                 .imageOffset = {0, 0, 0},
                                 .imageExtent = {width, rows, 1u}};
  vk_->CmdCopyImageToBuffer(command, texture_, VK_IMAGE_LAYOUT_GENERAL, transfer_.buffer, 1u,
                            &region);
  memoryBarrier(vk_->CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  if (!runOnce(command)) return false;
  const auto* texel = static_cast<const float*>(transfer_.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    std::uint32_t* to = first + static_cast<std::size_t>(row) * pitch;
    for (unsigned x = 0; x < width; ++x, texel += 4) {
      to[x] = (byteOf(texel[0]) << 16) | (byteOf(texel[1]) << 8) | byteOf(texel[2]);
    }
  }
  return true;
}

bool VulkanDevice::seed(const std::uint32_t* first, unsigned width, unsigned rows,
                        std::size_t pitch) {
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * rows * kTexelBytes;
  if (bytes > transfer_.size) {
    freeBuffer(transfer_);
    if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true, transfer_)) {
      freeBuffer(transfer_);
      return false;
    }
  }
  // Each channel as the shader writes it: the byte over 255.
  waitFor(drawFence_, drawSubmitted_);
  auto* texel = static_cast<float*>(transfer_.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    const std::uint32_t* from = first + static_cast<std::size_t>(row) * pitch;
    for (unsigned x = 0; x < width; ++x, texel += 4) {
      texel[0] = static_cast<float>((from[x] >> 16) & 0xFFu) / 255.0f;
      texel[1] = static_cast<float>((from[x] >> 8) & 0xFFu) / 255.0f;
      texel[2] = static_cast<float>(from[x] & 0xFFu) / 255.0f;
      texel[3] = 1.0f;
    }
  }
  VkCommandBuffer command = newCommand();
  const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .pNext = nullptr,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       .pInheritanceInfo = nullptr};
  if (command == VK_NULL_HANDLE || vk_->BeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    return false;
  }
  memoryBarrier(vk_->CmdPipelineBarrier, command,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  const VkBufferImageCopy region{.bufferOffset = 0u,
                                 .bufferRowLength = width,
                                 .bufferImageHeight = rows,
                                 .imageSubresource = kColorLayer,
                                 .imageOffset = {0, 0, 0},
                                 .imageExtent = {width, rows, 1u}};
  vk_->CmdCopyBufferToImage(command, transfer_.buffer, texture_, VK_IMAGE_LAYOUT_GENERAL, 1u,
                            &region);
  if (!runOnce(command)) return false;
  shapeWidth_ = width;
  shapeRows_ = rows;
  return true;
}

}  // namespace snaggletooth_libretro
