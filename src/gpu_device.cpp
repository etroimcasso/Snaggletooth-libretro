#include "gpu_device.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace snaggletooth_libretro {

namespace {

namespace gpu = snaggletooth::gpu;

constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
constexpr std::size_t kTexelBytes = 16u;
// What warm() maps and uploads: buffers of a length a frame with no VRAM write fits, and a frame
// header of zeros, whose height of 0 leaves every thread of each pass drawing nothing.
constexpr std::size_t kWarmBytes = 131072u;
constexpr std::size_t kWarmHeaderBytes = 128u;

[[nodiscard]] gpu::ShaderFormat toFormats(SDL_GPUShaderFormat sdl) noexcept {
  gpu::ShaderFormat out = gpu::ShaderFormat::None;
  if ((sdl & SDL_GPU_SHADERFORMAT_SPIRV) != 0u) out = out | gpu::ShaderFormat::Spirv;
  if ((sdl & SDL_GPU_SHADERFORMAT_DXIL) != 0u) out = out | gpu::ShaderFormat::Dxil;
  if ((sdl & SDL_GPU_SHADERFORMAT_METALLIB) != 0u) out = out | gpu::ShaderFormat::Metallib;
  return out;
}

[[nodiscard]] SDL_GPUShaderFormat toSdl(gpu::ShaderFormat format) noexcept {
  switch (format) {
    case gpu::ShaderFormat::Spirv: return SDL_GPU_SHADERFORMAT_SPIRV;
    case gpu::ShaderFormat::Dxil: return SDL_GPU_SHADERFORMAT_DXIL;
    case gpu::ShaderFormat::Metallib: return SDL_GPU_SHADERFORMAT_METALLIB;
    default: return SDL_GPU_SHADERFORMAT_INVALID;
  }
}

// The formats the library carries the shader in: the platform's one, the slot the build filled.
[[nodiscard]] SDL_GPUShaderFormat builtFormats() noexcept {
  const gpu::ShaderVariants& shader = gpu::ppuShader(gpu::Pass::Pixels);
  SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_INVALID;
  if (shader.spirv.data != nullptr) formats |= SDL_GPU_SHADERFORMAT_SPIRV;
  if (shader.dxil.data != nullptr) formats |= SDL_GPU_SHADERFORMAT_DXIL;
  if (shader.metallib.data != nullptr) formats |= SDL_GPU_SHADERFORMAT_METALLIB;
  return formats;
}

[[nodiscard]] std::uint32_t byteOf(float channel) noexcept {
  return static_cast<std::uint32_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
}

#if defined(__APPLE__)
// SDL's Cocoa code adds Objective-C categories to Apple's classes (NSCursor, NSScreen, NSWindow,
// NSApplication), and the runtime attaches them as soon as the module is mapped, before any entry
// point runs. Unmapping the module would leave those classes pointing into memory that is gone,
// and the frontend's next message to one of them would crash it. So the module keeps itself
// loaded from the moment it is mapped: a frontend that opens the core only to read its system
// information and closes it, then opens it again, gets the same module back. The module's
// symbols stay local: a frontend that finds retro_init across its whole process takes the core
// for one linked into itself and refuses to load it.
__attribute__((constructor)) void keepModuleLoaded() {
  Dl_info self{};
  if (dladdr(reinterpret_cast<const void*>(&keepModuleLoaded), &self) != 0) {
    dlopen(self.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD | RTLD_NODELETE);
  }
}
#endif

std::unique_ptr<GpuDevice> shared;
bool looked = false;        // a device has been looked for since the last close
bool sdlStarted = false;    // SDL is started for the device
std::string notOpened;      // SDL's reason where the look found none

}  // namespace

GpuDevice::~GpuDevice() {
  if (device_ == nullptr) return;
  SDL_WaitForGPUIdle(device_);
  if (mapped_ != nullptr) SDL_UnmapGPUTransferBuffer(device_, upload_);
  if (download_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, download_);
  if (upload_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, upload_);
  if (buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, buffer_);
  if (texture_ != nullptr) SDL_ReleaseGPUTexture(device_, texture_);
  if (scratch_ != nullptr) SDL_ReleaseGPUBuffer(device_, scratch_);
  for (SDL_GPUComputePipeline* pipeline : pipelines_) {
    if (pipeline != nullptr) SDL_ReleaseGPUComputePipeline(device_, pipeline);
  }
  SDL_DestroyGPUDevice(device_);
}

bool GpuDevice::open(std::string& error) {
  device_ = SDL_CreateGPUDevice(builtFormats(), false, nullptr);
  if (device_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  const SDL_GPUTextureCreateInfo info{
      .type = SDL_GPU_TEXTURETYPE_2D,
      .format = kTextureFormat,
      .usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
      .width = gpu::kTextureWidth,
      .height = gpu::kTextureHeight,
      .layer_count_or_depth = 1u,
      .num_levels = 1u,
      .sample_count = SDL_GPU_SAMPLECOUNT_1,
      .props = 0u,
  };
  texture_ = SDL_CreateGPUTexture(device_, &info);
  if (texture_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  const SDL_GPUBufferCreateInfo scratchInfo{
      .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
      .size = static_cast<Uint32>(gpu::kScratchBytes),
      .props = 0u};
  scratch_ = SDL_CreateGPUBuffer(device_, &scratchInfo);
  if (scratch_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  return true;
}

gpu::ShaderFormat GpuDevice::formats() const {
  return device_ == nullptr ? gpu::ShaderFormat::None : toFormats(SDL_GetGPUShaderFormats(device_));
}

bool GpuDevice::makePipeline(gpu::Pass pass, const gpu::ShaderBytecode& code,
                             gpu::ShaderFormat format, std::string& error) {
  const gpu::PassLayout layout = gpu::passLayout(pass);
  const SDL_GPUComputePipelineCreateInfo info{
      .code_size = code.size,
      .code = code.data,
      .entrypoint = code.entrypoint,
      .format = toSdl(format),
      .num_samplers = 0u,
      .num_readonly_storage_textures = 0u,
      .num_readonly_storage_buffers = layout.readOnlyBuffers,
      .num_readwrite_storage_textures = layout.readWriteTextures,
      .num_readwrite_storage_buffers = layout.readWriteBuffers,
      .num_uniform_buffers = layout.uniformBuffers,
      .threadcount_x = layout.threadsX,
      .threadcount_y = layout.threadsY,
      .threadcount_z = 1u,
      .props = 0u,
  };
  SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
  if (pipeline == nullptr) {
    error = SDL_GetError();
    return false;
  }
  pipelines_[static_cast<unsigned>(pass)] = pipeline;
  if (pipelines_[0] != nullptr && pipelines_[1] != nullptr && pipelines_[2] != nullptr) warm();
  return true;
}

void GpuDevice::warm() {
  const std::span<std::uint8_t> bytes = frameBytes(kWarmBytes);
  if (bytes.empty()) return;
  std::memset(bytes.data(), 0, kWarmHeaderBytes);
  SDL_UnmapGPUTransferBuffer(device_, upload_);
  mapped_ = nullptr;
  frame_ = gpu::Frame{.width = 256u, .height = 1u, .firstLine = 1u, .field = 0u,
                      .interlaced = false, .bytes = kWarmHeaderBytes};
  // The texture is made for the passes' writes alone, so it is not cleared here: every row the
  // core reads back is drawn by the frame it reads.
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  const SDL_GPUTransferBufferLocation from{.transfer_buffer = upload_, .offset = 0u};
  const SDL_GPUBufferRegion to{.buffer = buffer_, .offset = 0u, .size = static_cast<Uint32>(kWarmHeaderBytes)};
  SDL_UploadToGPUBuffer(copy, &from, &to, true);
  SDL_EndGPUCopyPass(copy);
  dispatch(command);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
  if (fence != nullptr) {
    SDL_WaitForGPUFences(device_, true, &fence, 1u);
    SDL_ReleaseGPUFence(device_, fence);
  }
  frame_ = gpu::Frame{};
}

std::span<std::uint8_t> GpuDevice::frameBytes(std::size_t bytes) {
  if (mapped_ != nullptr) {
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    mapped_ = nullptr;
  }
  // The buffers grow to the largest frame and never shrink.
  if (bytes > bufferBytes_) {
    if (upload_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    if (buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, buffer_);
    const SDL_GPUTransferBufferCreateInfo uploadInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = static_cast<Uint32>(bytes), .props = 0u};
    upload_ = SDL_CreateGPUTransferBuffer(device_, &uploadInfo);
    const SDL_GPUBufferCreateInfo bufferInfo{
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, .size = static_cast<Uint32>(bytes), .props = 0u};
    buffer_ = SDL_CreateGPUBuffer(device_, &bufferInfo);
    bufferBytes_ = bytes;
    if (upload_ == nullptr || buffer_ == nullptr) {
      bufferBytes_ = 0u;
      return {};
    }
  }
  mapped_ = SDL_MapGPUTransferBuffer(device_, upload_, true);
  if (mapped_ == nullptr) return {};
  return std::span<std::uint8_t>(static_cast<std::uint8_t*>(mapped_), bufferBytes_);
}

void GpuDevice::draw(const gpu::Frame& frame) {
  if (mapped_ == nullptr || pipelines_[0] == nullptr || pipelines_[1] == nullptr ||
      pipelines_[2] == nullptr) {
    return;
  }
  SDL_UnmapGPUTransferBuffer(device_, upload_);
  mapped_ = nullptr;
  frame_ = frame;
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  const SDL_GPUTransferBufferLocation from{.transfer_buffer = upload_, .offset = 0u};
  const SDL_GPUBufferRegion to{.buffer = buffer_, .offset = 0u, .size = static_cast<Uint32>(frame.bytes)};
  SDL_UploadToGPUBuffer(copy, &from, &to, true);
  SDL_EndGPUCopyPass(copy);
  dispatch(command);
  SDL_SubmitGPUCommandBuffer(command);
}

void GpuDevice::dispatch(SDL_GPUCommandBuffer* command) {
  // Each picture line on the texture row of its own number: the frontend's picture is placed
  // from the rows read back (Picture::place).
  const gpu::Uniform uniform = gpu::uniformFor(gpu::RowMap{});
  const SDL_GPUStorageBufferReadWriteBinding scratch{
      .buffer = scratch_, .cycle = false, .padding1 = 0u, .padding2 = 0u, .padding3 = 0u};
  for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
    const bool pixels = pass == gpu::Pass::Pixels;
    const SDL_GPUStorageTextureReadWriteBinding target{.texture = texture_,
                                                       .mip_level = 0u,
                                                       .layer = 0u,
                                                       .cycle = false,
                                                       .padding1 = 0u,
                                                       .padding2 = 0u,
                                                       .padding3 = 0u};
    SDL_GPUComputePass* run = SDL_BeginGPUComputePass(command, pixels ? &target : nullptr,
                                                      pixels ? 1u : 0u, &scratch, 1u);
    SDL_BindGPUComputePipeline(run, pipelines_[static_cast<unsigned>(pass)]);
    SDL_BindGPUComputeStorageBuffers(run, 0u, &buffer_, 1u);
    if (pixels) {
      SDL_PushGPUComputeUniformData(command, 0u, &uniform, static_cast<Uint32>(gpu::kUniformBytes));
    }
    const gpu::Groups groups = gpu::groupsFor(pass, frame_);
    SDL_DispatchGPUCompute(run, groups.x, groups.y, groups.z);
    SDL_EndGPUComputePass(run);
  }
}

bool GpuDevice::readBack(unsigned width, unsigned lines, std::uint32_t* first, std::size_t pitch) {
  const std::size_t texels = static_cast<std::size_t>(width) * lines;
  const std::size_t bytes = texels * kTexelBytes;
  if (bytes > downloadBytes_) {
    if (download_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, download_);
    const SDL_GPUTransferBufferCreateInfo info{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = static_cast<Uint32>(bytes), .props = 0u};
    download_ = SDL_CreateGPUTransferBuffer(device_, &info);
    downloadBytes_ = download_ == nullptr ? 0u : bytes;
    if (download_ == nullptr) return false;
  }
  SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
  const SDL_GPUTextureRegion region{.texture = texture_, .mip_level = 0u, .layer = 0u,
                                    .x = 0u, .y = 0u, .z = 0u, .w = width, .h = lines, .d = 1u};
  const SDL_GPUTextureTransferInfo into{
      .transfer_buffer = download_, .offset = 0u, .pixels_per_row = width, .rows_per_layer = lines};
  SDL_DownloadFromGPUTexture(copy, &region, &into);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
  if (fence == nullptr) return false;
  SDL_WaitForGPUFences(device_, true, &fence, 1u);
  SDL_ReleaseGPUFence(device_, fence);
  const auto* texel = static_cast<const float*>(SDL_MapGPUTransferBuffer(device_, download_, false));
  if (texel == nullptr) return false;
  for (unsigned line = 0; line < lines; ++line) {
    std::uint32_t* to = first + static_cast<std::size_t>(line) * pitch;
    for (unsigned x = 0; x < width; ++x, texel += 4) {
      to[x] = (byteOf(texel[0]) << 16) | (byteOf(texel[1]) << 8) | byteOf(texel[2]);
    }
  }
  SDL_UnmapGPUTransferBuffer(device_, download_);
  return true;
}

GpuDevice* sharedGpuDevice(std::string& error) {
  if (shared) return shared.get();
  if (looked) {
    error = notOpened;
    return nullptr;
  }
  looked = true;
  // The core runs inside the frontend's process: SDL starts without a main of its own, opens no
  // window, and leaves the process's signals to the frontend.
  SDL_SetMainReady();
#if defined(__APPLE__)
  // SDL offers Metal only under the Cocoa video driver. Cocoa leaves the frontend's application
  // and its delegate as they are, and a process with no application of its own stays out of the
  // Dock.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "cocoa", SDL_HINT_OVERRIDE);
  SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
#else
  // The offscreen video driver touches no window system and carries SDL's Vulkan loading;
  // Direct3D 12 needs no video driver at all.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
#endif
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  sdlStarted = true;
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    notOpened = SDL_GetError();
    error = notOpened;
    return nullptr;
  }
  auto device = std::make_unique<GpuDevice>();
  if (!device->open(notOpened)) {
    error = notOpened;
    return nullptr;
  }
  shared = std::move(device);
  return shared.get();
}

void closeSharedGpuDevice() {
  shared.reset();
  // The module's SDL serves the device alone, so all of it is shut down; the next start reads
  // the environment and the hints afresh.
  if (sdlStarted) SDL_Quit();
  sdlStarted = false;
  looked = false;
  notOpened.clear();
}

}  // namespace snaggletooth_libretro
