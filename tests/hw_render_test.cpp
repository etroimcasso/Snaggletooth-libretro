// The picture shown through the frontend's own graphics device, libretro's hardware-render
// context: the core asks for the platform's context at every load, whatever the Picture Thread
// option says; until the context is ready it asks the frontend to show the last frame again; once
// it is, every frame is a texture on the frontend's device holding the picture the machine draws
// on the processor — drawn on the GPU into it, at every shape and under every interlace option,
// or drawn on the processor and copied into it — and the picture carries on unchanged when the
// GPU is turned on or off while a game runs, when the frontend makes its context anew, and when a
// state saved on the processor loads.
//
// The suite is the hardware frontend of the platform: on Windows it makes a Direct3D 12 device
// and queue, the computer's graphics card or the software one Windows carries, and hands the
// core libretro's Direct3D 12 interface over them; on macOS and Linux it makes a Vulkan instance
// and device through the computer's Vulkan loader and hands over libretro's Vulkan interface. It
// reads back each texture the core hands it. What each frame should hold comes from a second
// machine run here on the processor (reference_pictures.h). A case that needs a device skips,
// saying why, on a computer where none opens.

#include <libretro.h>

#include "loaded_core.h"
#include "reference_pictures.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <libretro_d3d12.h>

#include <dxgi1_4.h>
#else
#include <libretro_vulkan.h>

#include <dlfcn.h>
#endif

namespace {

using snaggletooth_libretro_test::expectedFor;
using snaggletooth_libretro_test::expectPictures;
using snaggletooth_libretro_test::kColourBits;
using snaggletooth_libretro_test::kInterlaced;
using snaggletooth_libretro_test::kModeFive;
using snaggletooth_libretro_test::kModeZero;
using snaggletooth_libretro_test::kProgressive;
using snaggletooth_libretro_test::kTaller;
using snaggletooth_libretro_test::LoadedCore;
using snaggletooth_libretro_test::Mode;
using snaggletooth_libretro_test::Picture;
using snaggletooth_libretro_test::pictureCartridge;

constexpr const char* kInterlaceKey = "snaggletooth_interlace";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

// The context the core asks for on this platform, and what its messages begin with where it
// cannot use the device.
#ifdef _WIN32
constexpr retro_hw_context_type kContextType = RETRO_HW_CONTEXT_D3D12;
constexpr const char* kNoDevice = "Snaggletooth cannot use the frontend's Direct3D 12 device";
#else
constexpr retro_hw_context_type kContextType = RETRO_HW_CONTEXT_VULKAN;
constexpr const char* kNoDevice = "Snaggletooth cannot use the frontend's Vulkan device";
#endif
constexpr const char* kNoGpu = "Snaggletooth cannot draw on the GPU here";

#ifdef _WIN32

// The texture's bytes, red first, a byte a channel, at the pitch Direct3D aligns a buffer's rows
// to.
constexpr DXGI_FORMAT kShownFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr std::size_t kRowPitch = 512u * 4u;

template <typename Object>
void release(Object*& object) noexcept {
  if (object != nullptr) object->Release();
  object = nullptr;
}

// The Direct3D 12 device the suite stands in for a frontend with: the computer's graphics card,
// or the software device Windows carries where no card makes a Direct3D 12 device. d3d12.dll and
// dxgi.dll are opened at run time, so the suite links neither.
class D3d12Host {
 public:
  D3d12Host() { open(); }
  D3d12Host(const D3d12Host&) = delete;
  D3d12Host& operator=(const D3d12Host&) = delete;
  ~D3d12Host() {
    if (queue_ != nullptr && fence_ != nullptr) wait(signal());
    release(list_);
    release(allocator_);
    release(fence_);
    if (event_ != nullptr) CloseHandle(event_);
    release(queue_);
    release(device_);
    release(adapter_);
    release(factory_);
    if (dxgi_ != nullptr) FreeLibrary(dxgi_);
    if (d3d12_ != nullptr) FreeLibrary(d3d12_);
  }

  // Empty where the device is ready; otherwise why there is none.
  [[nodiscard]] const std::string& missing() const noexcept { return missing_; }

  // libretro's Direct3D 12 interface over the device, as a frontend hands it to a core.
  [[nodiscard]] const retro_hw_render_interface* face() const noexcept {
    return reinterpret_cast<const retro_hw_render_interface*>(&face_);
  }

  // The texture the core last handed over, read back: the top `rows` rows, `width` pixels each.
  [[nodiscard]] Picture readShown(unsigned width, unsigned rows) {
    Picture picture{.width = width, .height = rows,
                    .pixels = std::vector<std::uint32_t>(std::size_t{width} * rows)};
    if (shown_ == nullptr) return picture;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kRowPitch * rows;
    desc.Height = 1u;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&buffer)))) {
      return picture;
    }
    allocator_->Reset();
    list_->Reset(allocator_, nullptr);
    transition(shown_, kRequiredState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = shown_;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0u;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = buffer;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint.Format = shownFormat_;
    target.PlacedFootprint.Footprint.Width = width;
    target.PlacedFootprint.Footprint.Height = rows;
    target.PlacedFootprint.Footprint.Depth = 1u;
    target.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kRowPitch);
    const D3D12_BOX box{0u, 0u, 0u, width, rows, 1u};
    list_->CopyTextureRegion(&target, 0u, 0u, 0u, &source, &box);
    transition(shown_, D3D12_RESOURCE_STATE_COPY_SOURCE, kRequiredState);
    list_->Close();
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1u, lists);
    wait(signal());
    void* mapped = nullptr;
    if (SUCCEEDED(buffer->Map(0u, nullptr, &mapped))) {
      // Each texel lies as red, green, blue and alpha: an XRGB8888 word's bytes reversed.
      const auto* bytes = static_cast<const std::uint8_t*>(mapped);
      for (unsigned row = 0; row < rows; ++row) {
        const std::uint8_t* line = bytes + static_cast<std::size_t>(row) * kRowPitch;
        for (unsigned x = 0; x < width; ++x) {
          const std::uint8_t* texel = line + static_cast<std::size_t>(x) * 4u;
          picture.pixels[static_cast<std::size_t>(row) * width + x] =
              (static_cast<std::uint32_t>(texel[0]) << 16) |
              (static_cast<std::uint32_t>(texel[1]) << 8) | static_cast<std::uint32_t>(texel[2]);
        }
      }
      const D3D12_RANGE none{0u, 0u};
      buffer->Unmap(0u, &none);
    }
    release(buffer);
    for (std::uint32_t& pixel : picture.pixels) pixel &= kColourBits;
    return picture;
  }

  // The frontend moves on to its next frame as one is shown.
  void advance() noexcept {}

  // Whether the texture the core last handed over is `width` by `rows`, the picture's size: the
  // frontend shows the whole texture.
  [[nodiscard]] bool shownIsSized(unsigned width, unsigned rows) const {
    if (shown_ == nullptr) return false;
    const D3D12_RESOURCE_DESC desc = shown_->GetDesc();
    return desc.Width == width && desc.Height == rows;
  }

 private:
  // The state a frontend reads a handed texture in: RetroArch's Direct3D 12 driver samples it.
  static constexpr D3D12_RESOURCE_STATES kRequiredState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  void open() {
    using CreateFactory = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    d3d12_ = LoadLibraryW(L"d3d12.dll");
    dxgi_ = LoadLibraryW(L"dxgi.dll");
    if (d3d12_ == nullptr || dxgi_ == nullptr) {
      missing_ = "no Direct3D 12 here";
      return;
    }
    auto* createFactory = reinterpret_cast<CreateFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi_, "CreateDXGIFactory2")));
    auto* createDevice = reinterpret_cast<CreateDevice>(
        reinterpret_cast<void*>(GetProcAddress(d3d12_, "D3D12CreateDevice")));
    if (createFactory == nullptr || createDevice == nullptr ||
        FAILED(createFactory(0u, IID_PPV_ARGS(&factory_)))) {
      missing_ = "no DXGI factory";
      return;
    }
    // The computer's graphics card first, then the software device.
    for (UINT index = 0;; ++index) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory_->EnumAdapters1(index, &adapter) != S_OK) break;
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0u &&
          SUCCEEDED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        adapter_ = adapter;
        break;
      }
      release(adapter);
    }
    if (device_ == nullptr) {
      if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter_))) ||
          FAILED(createDevice(adapter_, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        missing_ = "no Direct3D 12 device was made";
        return;
      }
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_))) ||
        FAILED(device_->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator_))) ||
        FAILED(device_->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_, nullptr,
                                          IID_PPV_ARGS(&list_))) ||
        FAILED(list_->Close())) {
      missing_ = "the Direct3D 12 device made no queue";
      return;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    face_ = retro_hw_render_interface_d3d12{};
    face_.interface_type = RETRO_HW_RENDER_INTERFACE_D3D12;
    face_.interface_version = RETRO_HW_RENDER_INTERFACE_D3D12_VERSION;
    face_.handle = this;
    face_.device = device_;
    face_.queue = queue_;
    face_.D3DCompile = nullptr;
    face_.required_state = kRequiredState;
    face_.set_texture = [](void* handle, ID3D12Resource* texture, DXGI_FORMAT format) {
      auto* host = static_cast<D3d12Host*>(handle);
      host->shown_ = texture;
      host->shownFormat_ = format;
    };
  }

  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list_->ResourceBarrier(1u, &barrier);
  }
  [[nodiscard]] std::uint64_t signal() {
    queue_->Signal(fence_, ++fenceValue_);
    return fenceValue_;
  }
  void wait(std::uint64_t value) {
    if (fence_->GetCompletedValue() >= value || event_ == nullptr) return;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_))) WaitForSingleObject(event_, INFINITE);
  }

  HMODULE d3d12_ = nullptr;
  HMODULE dxgi_ = nullptr;
  std::string missing_;
  IDXGIFactory4* factory_ = nullptr;
  IDXGIAdapter* adapter_ = nullptr;
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE event_ = nullptr;
  std::uint64_t fenceValue_ = 0;
  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  retro_hw_render_interface_d3d12 face_{};
  ID3D12Resource* shown_ = nullptr;
  DXGI_FORMAT shownFormat_ = kShownFormat;
};

using Host = D3d12Host;

#else

// The frontend's swapchain, as the core sees it through the interface: two images in flight,
// sync indices 0 and 1, taken in turn.
constexpr std::uint32_t kSyncIndexMask = 0x3u;
constexpr std::uint32_t kSyncImages = 2u;

// The Vulkan device the suite stands in for a frontend with, made through the computer's loader.
class VulkanHost {
 public:
  VulkanHost() { open(); }
  VulkanHost(const VulkanHost&) = delete;
  VulkanHost& operator=(const VulkanHost&) = delete;
  ~VulkanHost() {
    if (device_ != VK_NULL_HANDLE) {
      DeviceWaitIdle(device_);
      if (pool_ != VK_NULL_HANDLE) DestroyCommandPool(device_, pool_, nullptr);
      DestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) DestroyInstance(instance_, nullptr);
    if (loader_ != nullptr) dlclose(loader_);
  }

  // Empty where the device is ready; otherwise why there is none.
  [[nodiscard]] const std::string& missing() const noexcept { return missing_; }

  // libretro's Vulkan interface over the device, as a frontend hands it to a core.
  [[nodiscard]] const retro_hw_render_interface* face() const noexcept {
    return reinterpret_cast<const retro_hw_render_interface*>(&face_);
  }

  // The image the core last handed over, read back: the top `rows` rows, `width` pixels each.
  [[nodiscard]] Picture readShown(unsigned width, unsigned rows) {
    Picture picture{.width = width, .height = rows,
                    .pixels = std::vector<std::uint32_t>(std::size_t{width} * rows)};
    if (shown_.image_view == VK_NULL_HANDLE) return picture;
    const VkImage image = shown_.create_info.image;
    const VkDeviceSize bytes = VkDeviceSize{width} * rows * 4u;
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .pNext = nullptr,
                                        .flags = 0u,
                                        .size = bytes,
                                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                        .queueFamilyIndexCount = 0u,
                                        .pQueueFamilyIndices = nullptr};
    VkBuffer buffer = VK_NULL_HANDLE;
    CreateBuffer(device_, &bufferInfo, nullptr, &buffer);
    VkMemoryRequirements needs{};
    GetBufferMemoryRequirements(device_, buffer, &needs);
    VkPhysicalDeviceMemoryProperties memory{};
    GetPhysicalDeviceMemoryProperties(gpu_, &memory);
    std::uint32_t type = 0;
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      if ((needs.memoryTypeBits & (1u << i)) != 0u &&
          (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
        type = i;
        break;
      }
    }
    const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .pNext = nullptr,
                                        .allocationSize = needs.size,
                                        .memoryTypeIndex = type};
    VkDeviceMemory held = VK_NULL_HANDLE;
    AllocateMemory(device_, &allocate, nullptr, &held);
    BindBufferMemory(device_, buffer, held, 0u);

    const VkCommandBufferAllocateInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u};
    VkCommandBuffer command = VK_NULL_HANDLE;
    AllocateCommandBuffers(device_, &commandInfo, &command);
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .pNext = nullptr,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                         .pInheritanceInfo = nullptr};
    BeginCommandBuffer(command, &begin);
    const auto transition = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags written,
                                VkAccessFlags read) {
      const VkImageMemoryBarrier barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .pNext = nullptr,
          .srcAccessMask = written,
          .dstAccessMask = read,
          .oldLayout = from,
          .newLayout = to,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = image,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
      CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                         &barrier);
    };
    transition(shown_.image_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{
        .bufferOffset = 0u,
        .bufferRowLength = width,
        .bufferImageHeight = rows,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, rows, 1u}};
    CmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1u, &region);
    transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shown_.image_layout,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    EndCommandBuffer(command);
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .pNext = nullptr,
                              .waitSemaphoreCount = 0u,
                              .pWaitSemaphores = nullptr,
                              .pWaitDstStageMask = nullptr,
                              .commandBufferCount = 1u,
                              .pCommandBuffers = &command,
                              .signalSemaphoreCount = 0u,
                              .pSignalSemaphores = nullptr};
    QueueSubmit(queue_, 1u, &submit, VK_NULL_HANDLE);
    QueueWaitIdle(queue_);
    void* mapped = nullptr;
    MapMemory(device_, held, 0u, bytes, 0u, &mapped);
    // Each pixel lies as blue, green, red and alpha, the bytes of an XRGB8888 word.
    std::memcpy(picture.pixels.data(), mapped, static_cast<std::size_t>(bytes));
    UnmapMemory(device_, held);
    FreeCommandBuffers(device_, pool_, 1u, &command);
    DestroyBuffer(device_, buffer, nullptr);
    FreeMemory(device_, held, nullptr);
    for (std::uint32_t& pixel : picture.pixels) pixel &= kColourBits;
    return picture;
  }

  // The frontend moves to its next swapchain image as a frame is shown.
  void advance() noexcept { ++frames_; }

  // Whether the image the core last handed over is `width` by `rows`, the picture's size: the
  // frontend shows the whole image. Vulkan does not answer an image's extent, so an image of that
  // size is made the way the core makes its own, and the two must take the same memory.
  [[nodiscard]] bool shownIsSized(unsigned width, unsigned rows) {
    if (shown_.image_view == VK_NULL_HANDLE) return false;
    VkMemoryRequirements handed{};
    GetImageMemoryRequirements(device_, shown_.create_info.image, &handed);
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .pNext = nullptr,
                                 .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = shown_.create_info.format,
                                 .extent = {width, rows, 1u},
                                 .mipLevels = 1u,
                                 .arrayLayers = 1u,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                 .queueFamilyIndexCount = 0u,
                                 .pQueueFamilyIndices = nullptr,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage reference = VK_NULL_HANDLE;
    if (CreateImage(device_, &info, nullptr, &reference) != VK_SUCCESS) return false;
    VkMemoryRequirements wanted{};
    GetImageMemoryRequirements(device_, reference, &wanted);
    DestroyImage(device_, reference, nullptr);
    return handed.size == wanted.size;
  }

 private:
  void open() {
#if defined(__APPLE__)
    for (const char* path : {"libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib",
                             "/usr/local/lib/libvulkan.1.dylib"}) {
      loader_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
      if (loader_ != nullptr) break;
    }
#else
    loader_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (loader_ == nullptr) {
      missing_ = "no Vulkan loader";
      return;
    }
    GetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader_, "vkGetInstanceProcAddr"));
    if (GetInstanceProcAddr == nullptr) {
      missing_ = "the Vulkan loader has no vkGetInstanceProcAddr";
      return;
    }
    const auto global = [&](const char* name) { return GetInstanceProcAddr(VK_NULL_HANDLE, name); };
    auto* createInstance = reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
    auto* instanceExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        global("vkEnumerateInstanceExtensionProperties"));
    // A loader that lists portability implementations, as MoltenVK is, enumerates them only to an
    // instance that asks for them.
    std::uint32_t count = 0;
    instanceExtensions(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> offered(count);
    instanceExtensions(nullptr, &count, offered.data());
    std::vector<const char*> extensions;
    VkInstanceCreateFlags flags = 0u;
    for (const VkExtensionProperties& extension : offered) {
      if (std::strcmp(extension.extensionName, "VK_KHR_portability_enumeration") == 0) {
        extensions.push_back("VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
      }
    }
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pNext = nullptr,
                                .pApplicationName = "snaggletooth_libretro_tests",
                                .applicationVersion = 1u,
                                .pEngineName = nullptr,
                                .engineVersion = 0u,
                                .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instanceInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = flags,
        .pApplicationInfo = &app,
        .enabledLayerCount = 0u,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data()};
    if (createInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
      missing_ = "the Vulkan loader made no instance";
      return;
    }
    const auto at = [&](const char* name) { return GetInstanceProcAddr(instance_, name); };
    DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(at("vkDestroyInstance"));
    auto* enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(at("vkEnumeratePhysicalDevices"));
    auto* families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        at("vkGetPhysicalDeviceQueueFamilyProperties"));
    auto* deviceExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        at("vkEnumerateDeviceExtensionProperties"));
    auto* createDevice = reinterpret_cast<PFN_vkCreateDevice>(at("vkCreateDevice"));
    GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        at("vkGetPhysicalDeviceMemoryProperties"));
    auto* deviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(at("vkGetDeviceProcAddr"));

    count = 0;
    enumerate(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    enumerate(instance_, &count, gpus.data());
    // The device's queue family that draws and computes, as a frontend's does.
    for (VkPhysicalDevice gpu : gpus) {
      std::uint32_t familyCount = 0;
      families(gpu, &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> properties(familyCount);
      families(gpu, &familyCount, properties.data());
      for (std::uint32_t family = 0; family < familyCount; ++family) {
        const VkQueueFlags both = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((properties[family].queueFlags & both) == both) {
          gpu_ = gpu;
          family_ = family;
          break;
        }
      }
      if (gpu_ != VK_NULL_HANDLE) break;
    }
    if (gpu_ == VK_NULL_HANDLE) {
      missing_ = "no Vulkan device draws and computes";
      return;
    }
    count = 0;
    deviceExtensions(gpu_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceOffered(count);
    deviceExtensions(gpu_, nullptr, &count, deviceOffered.data());
    std::vector<const char*> deviceWanted;
    for (const VkExtensionProperties& extension : deviceOffered) {
      if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) {
        deviceWanted.push_back("VK_KHR_portability_subset");
      }
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags = 0u,
                                            .queueFamilyIndex = family_,
                                            .queueCount = 1u,
                                            .pQueuePriorities = &priority};
    const VkDeviceCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0u,
        .queueCreateInfoCount = 1u,
        .pQueueCreateInfos = &queueInfo,
        .enabledLayerCount = 0u,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceWanted.size()),
        .ppEnabledExtensionNames = deviceWanted.data(),
        .pEnabledFeatures = nullptr};
    if (createDevice(gpu_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
      missing_ = "the Vulkan device was not made";
      return;
    }
    const auto in = [&](const char* name) { return deviceProcAddr(device_, name); };
    DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(in("vkDestroyDevice"));
    DeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(in("vkDeviceWaitIdle"));
    auto* getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(in("vkGetDeviceQueue"));
    getQueue(device_, family_, 0u, &queue_);
    auto* createPool = reinterpret_cast<PFN_vkCreateCommandPool>(in("vkCreateCommandPool"));
    DestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(in("vkDestroyCommandPool"));
    AllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(in("vkAllocateCommandBuffers"));
    FreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(in("vkFreeCommandBuffers"));
    BeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(in("vkBeginCommandBuffer"));
    EndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(in("vkEndCommandBuffer"));
    CmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(in("vkCmdPipelineBarrier"));
    CmdCopyImageToBuffer =
        reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(in("vkCmdCopyImageToBuffer"));
    QueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(in("vkQueueSubmit"));
    QueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(in("vkQueueWaitIdle"));
    CreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(in("vkCreateBuffer"));
    DestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(in("vkDestroyBuffer"));
    GetBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(in("vkGetBufferMemoryRequirements"));
    AllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(in("vkAllocateMemory"));
    FreeMemory = reinterpret_cast<PFN_vkFreeMemory>(in("vkFreeMemory"));
    BindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(in("vkBindBufferMemory"));
    MapMemory = reinterpret_cast<PFN_vkMapMemory>(in("vkMapMemory"));
    UnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(in("vkUnmapMemory"));
    CreateImage = reinterpret_cast<PFN_vkCreateImage>(in("vkCreateImage"));
    DestroyImage = reinterpret_cast<PFN_vkDestroyImage>(in("vkDestroyImage"));
    GetImageMemoryRequirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(in("vkGetImageMemoryRequirements"));
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family_};
    createPool(device_, &poolInfo, nullptr, &pool_);

    face_ = retro_hw_render_interface_vulkan{};
    face_.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    face_.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    face_.handle = this;
    face_.instance = instance_;
    face_.gpu = gpu_;
    face_.device = device_;
    face_.get_device_proc_addr = deviceProcAddr;
    face_.get_instance_proc_addr = GetInstanceProcAddr;
    face_.queue = queue_;
    face_.queue_index = family_;
    face_.set_image = [](void* handle, const retro_vulkan_image* image, uint32_t, const VkSemaphore*,
                         uint32_t) { static_cast<VulkanHost*>(handle)->shown_ = *image; };
    face_.get_sync_index = [](void* handle) {
      return static_cast<VulkanHost*>(handle)->frames_ % kSyncImages;
    };
    face_.get_sync_index_mask = [](void*) { return kSyncIndexMask; };
    face_.set_command_buffers = [](void*, uint32_t, const VkCommandBuffer*) {};
    face_.wait_sync_index = [](void* handle) {
      auto* host = static_cast<VulkanHost*>(handle);
      host->QueueWaitIdle(host->queue_);
    };
    face_.lock_queue = [](void*) {};
    face_.unlock_queue = [](void*) {};
    face_.set_signal_semaphore = [](void*, VkSemaphore) {};
  }

  void* loader_ = nullptr;
  std::string missing_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t family_ = 0;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  retro_hw_render_interface_vulkan face_{};
  retro_vulkan_image shown_{};
  std::uint32_t frames_ = 0;

  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkDestroyDevice DestroyDevice = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
};

using Host = VulkanHost;

#endif

// What the test frontend saw the core do, and what it answers the core with.
struct Frontend {
  std::map<std::string, std::string> values;  // the options the player set
  bool changed = false;                       // the options changed since the core last asked
  std::vector<std::string> messages;          // every SET_MESSAGE and SET_MESSAGE_EXT
  std::vector<Picture> pictures;              // each frame shown from an image
  std::size_t shownAgain = 0;                 // frames the core asked to have shown again
  std::size_t pixelFrames = 0;                // frames handed over as pixels in memory
  bool asked = false;                         // the core asked for a hardware context
  retro_hw_render_callback callback{};        // the callback it asked with
  Host* host = nullptr;                       // the device the frontend gives, or none

  // The first of the core's messages beginning with `start`, or an empty string.
  [[nodiscard]] std::string message(const char* start) const {
    for (const std::string& said : messages) {
      if (said.rfind(start, 0) == 0) return said;
    }
    return {};
  }
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* variable = static_cast<retro_variable*>(data);
      const auto value = frontend->values.find(variable->key);
      if (value == frontend->values.end()) return false;
      variable->value = value->second.c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = frontend->changed;
      frontend->changed = false;
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = 1u;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      frontend->messages.emplace_back(static_cast<const retro_message*>(data)->msg);
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      frontend->messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
      frontend->asked = true;
      frontend->callback = *static_cast<const retro_hw_render_callback*>(data);
      return frontend->callback.context_type == kContextType;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
      if (frontend->host == nullptr || !frontend->host->missing().empty()) return false;
      *static_cast<const retro_hw_render_interface**>(data) = frontend->host->face();
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  (void)pitch;
  if (data == nullptr) {
    ++frontend->shownAgain;
    return;
  }
  if (data != RETRO_HW_FRAME_BUFFER_VALID || frontend->host == nullptr) {
    ++frontend->pixelFrames;
    return;
  }
  frontend->pictures.push_back(frontend->host->readShown(width, height));
  frontend->host->advance();
}

void audioSample(std::int16_t left, std::int16_t right) {
  (void)left;
  (void)right;
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  (void)data;
  return frames;
}

void inputPoll() {}

std::int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)port;
  (void)device;
  (void)index;
  (void)id;
  return 0;
}

// The core loaded and set up as a frontend sets one up: the callbacks handed over, then
// retro_init. The frontend takes its context down and deinitializes the core when the case ends.
class Core {
 public:
  explicit Core(Frontend& seen) : seen_(seen) {
    frontend = &seen;
    if (!module_.loaded()) return;
    module_.entry<void(retro_environment_t)>("retro_set_environment")(environment);
    module_.entry<void(retro_video_refresh_t)>("retro_set_video_refresh")(videoRefresh);
    module_.entry<void(retro_audio_sample_t)>("retro_set_audio_sample")(audioSample);
    module_.entry<void(retro_audio_sample_batch_t)>("retro_set_audio_sample_batch")(
        audioSampleBatch);
    module_.entry<void(retro_input_poll_t)>("retro_set_input_poll")(inputPoll);
    module_.entry<void(retro_input_state_t)>("retro_set_input_state")(inputState);
    module_.entry<void()>("retro_init")();
  }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() {
    if (module_.loaded()) {
      module_.entry<void()>("retro_unload_game")();
      if (ready_ && seen_.callback.context_destroy != nullptr) seen_.callback.context_destroy();
      module_.entry<void()>("retro_deinit")();
    }
    frontend = nullptr;
  }

  [[nodiscard]] bool loaded() const noexcept { return module_.loaded(); }

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game{.path = nullptr, .data = image.data(), .size = image.size(),
                               .meta = nullptr};
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  // The frontend's context ready, as a frontend makes it after the cartridge loads.
  void contextReady() {
    ready_ = true;
    seen_.callback.context_reset();
  }
  // The frontend's context taken down, then made anew.
  void contextAnew() {
    seen_.callback.context_destroy();
    seen_.callback.context_reset();
  }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }
  [[nodiscard]] std::vector<std::uint8_t> serialize() const {
    std::vector<std::uint8_t> state(module_.entry<size_t()>("retro_serialize_size")());
    if (!module_.entry<bool(void*, size_t)>("retro_serialize")(state.data(), state.size())) {
      state.clear();
    }
    return state;
  }
  bool unserialize(const std::vector<std::uint8_t>& state) {
    return module_.entry<bool(const void*, size_t)>("retro_unserialize")(state.data(),
                                                                         state.size());
  }

 private:
  Frontend& seen_;
  LoadedCore module_;
  bool ready_ = false;
};

// Loads a cartridge under a frontend with the platform's device and makes the context ready.
// Skips, saying why, where the computer has no device, or where the core cannot use it.
#define LOAD_WITH_DEVICE(core, seen, host, rom)                                          \
  ASSERT_TRUE((core).loaded());                                                          \
  if (!(host).missing().empty()) GTEST_SKIP() << "no device here: " << (host).missing(); \
  ASSERT_TRUE((core).load(rom));                                                         \
  ASSERT_TRUE((seen).asked);                                                             \
  (core).contextReady();                                                                 \
  if (const std::string why = (seen).message(kNoDevice); !why.empty()) {                 \
    GTEST_SKIP() << why;                                                                 \
  }

// Runs a cartridge `runs` frames with the Picture Thread option and the interlace option set
// where each names a value, and holds every picture to the processor's.
void expectPictures(const std::vector<std::uint8_t>& rom, std::size_t runs, Mode mode,
                    const char* interlace, const char* pictureThread) {
  Host host;
  Frontend seen;
  seen.host = &host;
  if (interlace != nullptr) seen.values[kInterlaceKey] = interlace;
  if (pictureThread != nullptr) seen.values[kPictureThreadKey] = pictureThread;
  Core core(seen);
  LOAD_WITH_DEVICE(core, seen, host, rom);
  core.run(runs);
  EXPECT_EQ(seen.shownAgain, 0u);
  EXPECT_EQ(seen.pixelFrames, 0u);
  EXPECT_TRUE(seen.message(kNoGpu).empty()) << seen.message(kNoGpu);
  expectPictures(seen.pictures, expectedFor(rom, runs, mode));
}

TEST(HwRender, TheCoreAsksForThePlatformsContextWhateverThePictureThread) {
  for (const char* pictureThread : {"gpu", "enabled", "disabled"}) {
    Frontend seen;
    seen.values[kPictureThreadKey] = pictureThread;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
    EXPECT_TRUE(seen.asked) << pictureThread;
    EXPECT_EQ(seen.callback.context_type, kContextType) << pictureThread;
    EXPECT_NE(seen.callback.context_reset, nullptr) << pictureThread;
    EXPECT_NE(seen.callback.context_destroy, nullptr) << pictureThread;
  }
}

TEST(HwRender, BeforeTheContextIsReadyTheLastFrameIsShownAgain) {
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(pictureCartridge(kModeZero, kProgressive)));
  core.run(3u);
  EXPECT_EQ(seen.shownAgain, 3u);
  EXPECT_EQ(seen.pixelFrames, 0u);
  EXPECT_TRUE(seen.pictures.empty());
}

// The frontend shows the whole of a handed texture, so the texture is the picture's size: 256 by
// 224 for a plain frame, 512 by 448 for a woven interlaced picture in half-pixels.
TEST(HwRender, TheHandedTextureIsThePicturesSize) {
  {
    Host host;
    Frontend seen;
    seen.host = &host;
    Core core(seen);
    const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
    LOAD_WITH_DEVICE(core, seen, host, rom);
    core.run(2u);
    EXPECT_TRUE(host.shownIsSized(256u, 224u));
  }
  Host host;
  Frontend seen;
  seen.host = &host;
  seen.values[kInterlaceKey] = "weave";
  Core core(seen);
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kInterlaced);
  LOAD_WITH_DEVICE(core, seen, host, rom);
  core.run(3u);
  EXPECT_TRUE(host.shownIsSized(512u, 448u));
}

TEST(HwRender, GpuFramesReachTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeZero, kProgressive), 8u, Mode::Weave, nullptr, nullptr);
}

TEST(HwRender, GpuHalfPixelFramesReachTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kProgressive), 6u, Mode::Weave, nullptr, nullptr);
}

TEST(HwRender, GpuTallerPictureReachesTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeZero, kTaller), 6u, Mode::Weave, nullptr, nullptr);
}

TEST(HwRender, GpuWeaveOnTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Weave, "weave", nullptr);
}

TEST(HwRender, GpuBobOnTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Bob, "bob", nullptr);
}

TEST(HwRender, GpuOffOnTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Off, "off", nullptr);
}

TEST(HwRender, ProcessorFramesReachTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Weave, "weave", "disabled");
}

TEST(HwRender, PictureThreadFramesReachTheFrontendsDevice) {
  expectPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Weave, "weave", "enabled");
}

// The GPU turned off and on again while an interlaced game runs: every picture is the one a
// steady run shows, the woven picture carried from the GPU's texture to the processor and back.
TEST(HwRender, TurningTheGpuOffAndOnKeepsThePicture) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kInterlaced);
  Host host;
  Frontend seen;
  seen.host = &host;
  seen.values[kInterlaceKey] = "weave";
  Core core(seen);
  LOAD_WITH_DEVICE(core, seen, host, rom);
  core.run(5u);
  for (const char* pictureThread : {"disabled", "gpu", "enabled", "gpu"}) {
    seen.values[kPictureThreadKey] = pictureThread;
    seen.changed = true;
    core.run(3u);
  }
  EXPECT_EQ(seen.shownAgain, 0u);
  expectPictures(seen.pictures, expectedFor(rom, 17u, Mode::Weave));
}

// The frontend takes its context down and makes it anew mid-game, as it does when its video
// driver restarts: the game and its woven picture carry on.
TEST(HwRender, AContextMadeAnewCarriesOn) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeFive, kInterlaced);
  Host host;
  Frontend seen;
  seen.host = &host;
  seen.values[kInterlaceKey] = "weave";
  Core core(seen);
  LOAD_WITH_DEVICE(core, seen, host, rom);
  core.run(5u);
  core.contextAnew();
  core.run(7u);
  EXPECT_EQ(seen.shownAgain, 0u);
  EXPECT_TRUE(seen.message(kNoDevice).empty());
  expectPictures(seen.pictures, expectedFor(rom, 12u, Mode::Weave));
}

TEST(HwRender, AStateSavedOnTheProcessorLoadsOnTheFrontendsDevice) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  Host host;
  std::vector<std::uint8_t> state;
  std::vector<Picture> followed;
  {
    Frontend seen;
    seen.host = &host;
    seen.values[kPictureThreadKey] = "disabled";
    Core core(seen);
    LOAD_WITH_DEVICE(core, seen, host, rom);
    core.run(4u);
    state = core.serialize();
    ASSERT_FALSE(state.empty());
    seen.pictures.clear();
    core.run(8u);
    followed = seen.pictures;
  }
  Frontend seen;
  seen.host = &host;
  Core core(seen);
  LOAD_WITH_DEVICE(core, seen, host, rom);
  // A frame is shown before the state loads. The first picture after the load keeps that frame's
  // field in its other rows, as a woven picture does on the processor too; every later picture
  // is two fields drawn after the load.
  core.run();
  ASSERT_TRUE(core.unserialize(state));
  seen.pictures.clear();
  core.run(8u);
  ASSERT_EQ(seen.pictures.size(), followed.size());
  for (std::size_t at = 1; at < followed.size(); ++at) {
    EXPECT_TRUE(seen.pictures[at] == followed[at]) << "frame " << at << " after the load";
  }
}

}  // namespace
