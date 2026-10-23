#include "d3d12_device.h"

#include <cstring>

namespace snaggletooth_libretro {

namespace {

namespace gpu = snaggletooth::gpu;

// The texture the passes draw into and the textures the frontend is handed, a byte a channel,
// red first: the Pixels pass's float stores land as the bytes the converter made, and a copy
// between the two needs one format. An XRGB8888 word lies in memory as blue, green, red and a
// spare byte, so a picture copied in or out swaps red and blue on the way.
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr std::size_t kTexelBytes = 4u;
// A row of the widest picture, the pitch every copy to or from a buffer uses; a whole number of
// the 256 bytes Direct3D aligns a buffer's rows to.
constexpr std::size_t kRowPitch = static_cast<std::size_t>(gpu::kTextureWidth) * kTexelBytes;
constexpr std::size_t kPictureBytes = kRowPitch * gpu::kTextureHeight;
static_assert(kRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0u);
// The textures the frontend is handed in turn, as many as the frontend may still be showing while
// the core fills the next.
constexpr std::size_t kShownTextures = 3u;
// The uniform's buffer, the size Direct3D asks of a constant buffer.
constexpr std::size_t kUniformBufferBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
// What warm() draws: a frame header of zeros, whose height of 0 leaves every thread of each pass
// drawing nothing, in buffers of a length a frame with no VRAM write fits.
constexpr std::size_t kWarmBytes = 131072u;
constexpr std::size_t kWarmHeaderBytes = 128u;

// Where each pass's bindings sit in its root signature, from the shaders' registers: the frame at
// t0 of space 0 in every pass, the scratch buffer at u0 of space 1 for Sprites and Tiles, and for
// Pixels the texture at u0 and the scratch buffer at u1 of space 1, through one table, with the
// row map at b0 of space 2.
constexpr unsigned kFrameParameter = 0u;
constexpr unsigned kScratchParameter = 1u;
constexpr unsigned kPixelsTableParameter = 1u;
constexpr unsigned kUniformParameter = 2u;
// The two slots of the shader-visible heap: the texture's view, then the scratch buffer's.
constexpr unsigned kTextureSlot = 0u;
constexpr unsigned kScratchSlot = 1u;

// The formats the library carries the shader in: the platform's one, the slot the build filled.
gpu::ShaderFormat builtFormats() noexcept {
  const gpu::ShaderVariants& shader = gpu::ppuShader(gpu::Pass::Pixels);
  gpu::ShaderFormat formats = gpu::ShaderFormat::None;
  if (shader.spirv.data != nullptr) formats = formats | gpu::ShaderFormat::Spirv;
  if (shader.dxil.data != nullptr) formats = formats | gpu::ShaderFormat::Dxil;
  if (shader.metallib.data != nullptr) formats = formats | gpu::ShaderFormat::Metallib;
  return formats;
}

template <typename Object>
void release(Object*& object) noexcept {
  if (object != nullptr) object->Release();
  object = nullptr;
}

// The footprint of the top `rows` rows, `width` texels each, in a buffer at the texture's pitch.
D3D12_TEXTURE_COPY_LOCATION bufferRows(ID3D12Resource* buffer, unsigned width, unsigned rows) {
  D3D12_TEXTURE_COPY_LOCATION location{};
  location.pResource = buffer;
  location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location.PlacedFootprint.Offset = 0u;
  location.PlacedFootprint.Footprint.Format = kTextureFormat;
  location.PlacedFootprint.Footprint.Width = width;
  location.PlacedFootprint.Footprint.Height = rows;
  location.PlacedFootprint.Footprint.Depth = 1u;
  location.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kRowPitch);
  return location;
}

D3D12_TEXTURE_COPY_LOCATION textureLevel(ID3D12Resource* texture) {
  D3D12_TEXTURE_COPY_LOCATION location{};
  location.pResource = texture;
  location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  location.SubresourceIndex = 0u;
  return location;
}

// An XRGB8888 word from a texel's bytes, and a texel's bytes from a word made opaque.
[[nodiscard]] std::uint32_t wordOf(const std::uint8_t* texel) noexcept {
  return (static_cast<std::uint32_t>(texel[0]) << 16) | (static_cast<std::uint32_t>(texel[1]) << 8) |
         static_cast<std::uint32_t>(texel[2]);
}
[[nodiscard]] std::uint32_t texelOf(std::uint32_t word) noexcept {
  return (word & 0x0000FF00u) | ((word >> 16) & 0xFFu) | ((word & 0xFFu) << 16) | 0xFF000000u;
}

}  // namespace

D3d12Device::~D3d12Device() {
  if (!opened_) return;
  // Everything the device made is let go once the queue is done with it.
  if (fence_ != nullptr) waitFor(fenceValue_);
  for (Shown& shown : shown_) {
    release(shown.list);
    release(shown.allocator);
    freeBuffer(shown.staging);
    release(shown.texture);
  }
  release(drawList_);
  release(drawAllocator_);
  for (ID3D12PipelineState*& pipeline : pipelines_) release(pipeline);
  for (ID3D12RootSignature*& signature : rootSignatures_) release(signature);
  release(clearHeap_);
  release(shaderHeap_);
  freeBuffer(readback_);
  freeBuffer(upload_);
  freeBuffer(uniform_);
  freeBuffer(frame_);
  release(scratch_);
  release(texture_);
  release(fence_);
  if (fenceEvent_ != nullptr) CloseHandle(fenceEvent_);
  if (d3d12_ != nullptr) FreeLibrary(d3d12_);
}

void D3d12Device::abandon() noexcept { opened_ = false; }

bool D3d12Device::makeBuffer(std::size_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                             D3D12_RESOURCE_FLAGS flags, Buffer& out) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = heap;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1u;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1u;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  if (FAILED(frontend_.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                       state, nullptr,
                                                       IID_PPV_ARGS(&out.resource)))) {
    return false;
  }
  if (heap != D3D12_HEAP_TYPE_DEFAULT) {
    // An upload buffer stays mapped for the processor to write; a readback buffer stays mapped
    // for it to read once the copy into it is done.
    const D3D12_RANGE none{0u, 0u};
    if (FAILED(out.resource->Map(0u, heap == D3D12_HEAP_TYPE_READBACK ? nullptr : &none,
                                 &out.mapped))) {
      return false;
    }
  }
  out.size = size;
  return true;
}

void D3d12Device::freeBuffer(Buffer& buffer) noexcept {
  // Releasing the resource unmaps it.
  release(buffer.resource);
  buffer = Buffer{};
}

bool D3d12Device::makeTexture(unsigned width, unsigned height, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags, ID3D12Resource*& out) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1u;
  desc.MipLevels = 1u;
  desc.Format = kTextureFormat;
  desc.SampleDesc.Count = 1u;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return SUCCEEDED(frontend_.device->CreateCommittedResource(
      &properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)));
}

bool D3d12Device::makeList(ID3D12CommandAllocator*& allocator, ID3D12GraphicsCommandList*& list) {
  // A list is made recording and closed at once, so every use begins with a reset.
  return SUCCEEDED(frontend_.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))) &&
         SUCCEEDED(frontend_.device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       allocator, nullptr, IID_PPV_ARGS(&list))) &&
         SUCCEEDED(list->Close());
}

bool D3d12Device::makeRootSignature(gpu::Pass pass, std::string& error) {
  using Serialize = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                     ID3DBlob**, ID3DBlob**);
  auto* serialize = reinterpret_cast<Serialize>(
      reinterpret_cast<void*>(GetProcAddress(d3d12_, "D3D12SerializeRootSignature")));
  if (serialize == nullptr) {
    error = "d3d12.dll has no D3D12SerializeRootSignature";
    return false;
  }
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[kFrameParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[kFrameParameter].Descriptor.ShaderRegister = 0u;
  parameters[kFrameParameter].Descriptor.RegisterSpace = 0u;
  parameters[kFrameParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE pixelsRange{};
  unsigned count = 2u;
  if (pass == gpu::Pass::Pixels) {
    // A texture's view cannot be a root descriptor, so the texture and the scratch buffer go
    // through one table of two.
    pixelsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    pixelsRange.NumDescriptors = 2u;
    pixelsRange.BaseShaderRegister = 0u;
    pixelsRange.RegisterSpace = 1u;
    pixelsRange.OffsetInDescriptorsFromTableStart = 0u;
    parameters[kPixelsTableParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[kPixelsTableParameter].DescriptorTable.NumDescriptorRanges = 1u;
    parameters[kPixelsTableParameter].DescriptorTable.pDescriptorRanges = &pixelsRange;
    parameters[kPixelsTableParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[kUniformParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[kUniformParameter].Descriptor.ShaderRegister = 0u;
    parameters[kUniformParameter].Descriptor.RegisterSpace = 2u;
    parameters[kUniformParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    count = 3u;
  } else {
    parameters[kScratchParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[kScratchParameter].Descriptor.ShaderRegister = 0u;
    parameters[kScratchParameter].Descriptor.RegisterSpace = 1u;
    parameters[kScratchParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = count;
  desc.pParameters = parameters;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ID3DBlob* blob = nullptr;
  ID3DBlob* problem = nullptr;
  const HRESULT serialized = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &problem);
  if (FAILED(serialized)) {
    error = problem != nullptr ? std::string(static_cast<const char*>(problem->GetBufferPointer()),
                                             problem->GetBufferSize())
                               : std::string("the pass's root signature did not serialize");
    release(problem);
    release(blob);
    return false;
  }
  release(problem);
  const HRESULT made = frontend_.device->CreateRootSignature(
      0u, blob->GetBufferPointer(), blob->GetBufferSize(),
      IID_PPV_ARGS(&rootSignatures_[static_cast<unsigned>(pass)]));
  release(blob);
  if (FAILED(made)) {
    error = "the frontend's Direct3D 12 device made no root signature";
    return false;
  }
  return true;
}

std::uint64_t D3d12Device::execute(ID3D12GraphicsCommandList* list) {
  ID3D12CommandList* lists[] = {list};
  frontend_.queue->ExecuteCommandLists(1u, lists);
  const std::uint64_t value = ++fenceValue_;
  frontend_.queue->Signal(fence_, value);
  return value;
}

void D3d12Device::waitFor(std::uint64_t fenceValue) {
  if (fenceValue == 0u || fence_->GetCompletedValue() >= fenceValue) return;
  if (SUCCEEDED(fence_->SetEventOnCompletion(fenceValue, fenceEvent_))) {
    WaitForSingleObject(fenceEvent_, INFINITE);
  }
}

bool D3d12Device::runOnce() {
  if (FAILED(drawList_->Close())) return false;
  drawFenceValue_ = execute(drawList_);
  waitFor(drawFenceValue_);
  return true;
}

void D3d12Device::transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1u, &barrier);
}

bool D3d12Device::open(std::string& error) {
  if (frontend_.device == nullptr || frontend_.queue == nullptr) {
    error = "the frontend's Direct3D 12 interface names no device or queue";
    return false;
  }
  d3d12_ = LoadLibraryW(L"d3d12.dll");
  if (d3d12_ == nullptr) {
    error = "d3d12.dll did not open";
    return false;
  }
  opened_ = true;
  ID3D12Device* device = frontend_.device;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
  support.Format = kTextureFormat;
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
      (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) == 0u ||
      (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0u) {
    error = "the frontend's Direct3D 12 device cannot write the picture's texture";
    return false;
  }

  if (FAILED(device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    error = "the frontend's Direct3D 12 device made no fence";
    return false;
  }
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fenceEvent_ == nullptr) {
    error = "no event for the fence";
    return false;
  }
  if (!makeList(drawAllocator_, drawList_)) {
    error = "the frontend's Direct3D 12 device made no command list";
    return false;
  }

  if (!makeTexture(gpu::kTextureWidth, gpu::kTextureHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, texture_)) {
    error = "the frontend's Direct3D 12 device made no texture for the picture";
    return false;
  }
  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC scratchDesc{};
  scratchDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  scratchDesc.Width = gpu::kScratchBytes;
  scratchDesc.Height = 1u;
  scratchDesc.DepthOrArraySize = 1u;
  scratchDesc.MipLevels = 1u;
  scratchDesc.Format = DXGI_FORMAT_UNKNOWN;
  scratchDesc.SampleDesc.Count = 1u;
  scratchDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  scratchDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &scratchDesc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&scratch_))) ||
      !makeBuffer(kUniformBufferBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                  D3D12_RESOURCE_FLAG_NONE, uniform_)) {
    error = "the frontend's Direct3D 12 device made no buffers for the picture";
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 2u;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  D3D12_DESCRIPTOR_HEAP_DESC clearDesc = heapDesc;
  clearDesc.NumDescriptors = 1u;
  clearDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&shaderHeap_))) ||
      FAILED(device->CreateDescriptorHeap(&clearDesc, IID_PPV_ARGS(&clearHeap_)))) {
    error = "the frontend's Direct3D 12 device made no descriptor heap";
    return false;
  }
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_UNORDERED_ACCESS_VIEW_DESC textureView{};
  textureView.Format = kTextureFormat;
  textureView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  D3D12_UNORDERED_ACCESS_VIEW_DESC scratchView{};
  scratchView.Format = DXGI_FORMAT_R32_TYPELESS;
  scratchView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  scratchView.Buffer.NumElements = static_cast<UINT>(gpu::kScratchBytes / 4u);
  scratchView.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += static_cast<SIZE_T>(kTextureSlot) * stride;
  device->CreateUnorderedAccessView(texture_, nullptr, &textureView, slot);
  slot = shaderHeap_->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += static_cast<SIZE_T>(kScratchSlot) * stride;
  device->CreateUnorderedAccessView(scratch_, nullptr, &scratchView, slot);
  device->CreateUnorderedAccessView(texture_, nullptr, &textureView,
                                    clearHeap_->GetCPUDescriptorHandleForHeapStart());

  for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
    if (!makeRootSignature(pass, error)) return false;
  }

  // The texture starts black.
  if (FAILED(drawAllocator_->Reset()) || FAILED(drawList_->Reset(drawAllocator_, nullptr))) {
    error = "the frontend's Direct3D 12 device made no command list";
    return false;
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ID3D12DescriptorHeap* heaps[] = {shaderHeap_};
  drawList_->SetDescriptorHeaps(1u, heaps);
  D3D12_GPU_DESCRIPTOR_HANDLE textureGpu = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
  textureGpu.ptr += static_cast<UINT64>(kTextureSlot) * stride;
  drawList_->ClearUnorderedAccessViewFloat(textureGpu, clearHeap_->GetCPUDescriptorHandleForHeapStart(),
                                           texture_, black, 0u, nullptr);
  if (!runOnce()) {
    error = "the frontend's Direct3D 12 queue did not run the core's work";
    return false;
  }
  return true;
}

gpu::ShaderFormat D3d12Device::formats() const {
  // The library's own format, DXIL on Windows, so the machine hands over the bytes the library
  // carries and makePipeline() takes them as they are.
  return opened_ ? builtFormats() : gpu::ShaderFormat::None;
}

bool D3d12Device::makePipeline(gpu::Pass pass, const gpu::ShaderBytecode& code,
                               gpu::ShaderFormat format, std::string& error) {
  if (format != gpu::ShaderFormat::Dxil || code.data == nullptr) {
    error = "the frontend's Direct3D 12 device takes DXIL alone";
    return false;
  }
  const unsigned index = static_cast<unsigned>(pass);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rootSignatures_[index];
  desc.CS.pShaderBytecode = code.data;
  desc.CS.BytecodeLength = code.size;
  if (FAILED(frontend_.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines_[index])))) {
    pipelines_[index] = nullptr;
    error = "the frontend's Direct3D 12 device made no compute pipeline";
    return false;
  }
  if (pipelines_[0] != nullptr && pipelines_[1] != nullptr && pipelines_[2] != nullptr) warm();
  return true;
}

std::span<std::uint8_t> D3d12Device::frameBytes(std::size_t bytes) {
  // The passes of the frame before read the buffer, so it is written only once they are done.
  waitFor(drawFenceValue_);
  // The buffer grows to the largest frame and never shrinks.
  if (bytes > frame_.size) {
    freeBuffer(frame_);
    if (!makeBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                    D3D12_RESOURCE_FLAG_NONE, frame_)) {
      freeBuffer(frame_);
      return {};
    }
  }
  return std::span<std::uint8_t>(static_cast<std::uint8_t*>(frame_.mapped), frame_.size);
}

void D3d12Device::record(const gpu::Frame& frame, bool clear) {
  const UINT stride =
      frontend_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ID3D12DescriptorHeap* heaps[] = {shaderHeap_};
  drawList_->SetDescriptorHeaps(1u, heaps);
  D3D12_GPU_DESCRIPTOR_HANDLE table = shaderHeap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(kTextureSlot) * stride;
  if (clear) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    drawList_->ClearUnorderedAccessViewFloat(
        table, clearHeap_->GetCPUDescriptorHandleForHeapStart(), texture_, black, 0u, nullptr);
    D3D12_RESOURCE_BARRIER cleared{};
    cleared.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cleared.UAV.pResource = texture_;
    drawList_->ResourceBarrier(1u, &cleared);
  }
  for (const gpu::Pass pass : {gpu::Pass::Sprites, gpu::Pass::Tiles, gpu::Pass::Pixels}) {
    const unsigned index = static_cast<unsigned>(pass);
    const bool pixels = pass == gpu::Pass::Pixels;
    drawList_->SetComputeRootSignature(rootSignatures_[index]);
    drawList_->SetPipelineState(pipelines_[index]);
    drawList_->SetComputeRootShaderResourceView(kFrameParameter,
                                                frame_.resource->GetGPUVirtualAddress());
    if (pixels) {
      drawList_->SetComputeRootDescriptorTable(kPixelsTableParameter, table);
      drawList_->SetComputeRootConstantBufferView(kUniformParameter,
                                                  uniform_.resource->GetGPUVirtualAddress());
    } else {
      drawList_->SetComputeRootUnorderedAccessView(kScratchParameter,
                                                   scratch_->GetGPUVirtualAddress());
    }
    const gpu::Groups groups = gpu::groupsFor(pass, frame);
    drawList_->Dispatch(groups.x, groups.y, groups.z);
    // The next pass reads what this one wrote.
    D3D12_RESOURCE_BARRIER written[2] = {};
    written[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[0].UAV.pResource = scratch_;
    written[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[1].UAV.pResource = texture_;
    drawList_->ResourceBarrier(2u, written);
  }
}

void D3d12Device::draw(const gpu::Frame& frame) {
  if (frame_.mapped == nullptr || pipelines_[0] == nullptr || pipelines_[1] == nullptr ||
      pipelines_[2] == nullptr) {
    return;
  }
  waitFor(drawFenceValue_);
  // Each field at the rows the interlace mode places it at. A woven or doubled picture of a new
  // shape starts black, as the processor's picture does (Picture::place).
  const gpu::RowMap rows = gpu::rowMapFor(placement_, frame.interlaced, frame.field);
  const gpu::Uniform uniform = gpu::uniformFor(rows);
  std::memcpy(uniform_.mapped, &uniform, sizeof uniform);
  const unsigned shapeRows = gpu::rowsUnder(rows, frame.height);
  const bool clear = rows.step == 2u && (frame.width != shapeWidth_ || shapeRows != shapeRows_);
  shapeWidth_ = frame.width;
  shapeRows_ = shapeRows;

  if (FAILED(drawAllocator_->Reset()) || FAILED(drawList_->Reset(drawAllocator_, nullptr))) return;
  record(frame, clear);
  if (FAILED(drawList_->Close())) return;
  drawFenceValue_ = execute(drawList_);
}

void D3d12Device::warm() {
  const std::span<std::uint8_t> bytes = frameBytes(kWarmBytes);
  if (bytes.empty()) return;
  std::memset(bytes.data(), 0, kWarmHeaderBytes);
  draw(gpu::Frame{.width = 256u, .height = 1u, .firstLine = 1u, .field = 0u,
                  .interlaced = false, .bytes = kWarmHeaderBytes});
  waitFor(drawFenceValue_);
  shapeWidth_ = 0u;
  shapeRows_ = 0u;
}

D3d12Device::Shown* D3d12Device::shownForThisFrame(unsigned width, unsigned rows) {
  while (shown_.size() < kShownTextures) {
    Shown shown;
    if (!makeBuffer(kPictureBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                    D3D12_RESOURCE_FLAG_NONE, shown.staging) ||
        !makeList(shown.allocator, shown.list)) {
      shown_.push_back(shown);
      return nullptr;
    }
    shown_.push_back(shown);
  }
  Shown& shown = shown_[nextShown_];
  nextShown_ = (nextShown_ + 1u) % shown_.size();
  if (shown.staging.mapped == nullptr || shown.list == nullptr) return nullptr;
  // The core's last copy into this texture is done; the frontend's reads of it were queued
  // before this frame's copy, on the same queue, so they are done first.
  waitFor(shown.fenceValue);
  // The texture is the picture's size, so the frontend shows the picture and nothing beside it;
  // a picture of a new shape gets a new texture. A handed texture lives in the state the
  // frontend reads it in, and returns to it after every copy into it.
  if (shown.width != width || shown.height != rows) {
    release(shown.texture);
    shown.width = 0u;
    shown.height = 0u;
    if (!makeTexture(width, rows, frontend_.required_state, D3D12_RESOURCE_FLAG_NONE,
                     shown.texture)) {
      return nullptr;
    }
    shown.width = width;
    shown.height = rows;
  }
  if (FAILED(shown.allocator->Reset()) || FAILED(shown.list->Reset(shown.allocator, nullptr))) {
    return nullptr;
  }
  return &shown;
}

bool D3d12Device::copyRows(ID3D12GraphicsCommandList* list, ID3D12Resource* from,
                           ID3D12Resource* to, bool fromBuffer, unsigned width, unsigned rows) {
  const D3D12_TEXTURE_COPY_LOCATION source =
      fromBuffer ? bufferRows(from, width, rows) : textureLevel(from);
  const D3D12_TEXTURE_COPY_LOCATION target =
      fromBuffer ? textureLevel(to) : bufferRows(to, width, rows);
  const D3D12_BOX box{0u, 0u, 0u, width, rows, 1u};
  list->CopyTextureRegion(&target, 0u, 0u, 0u, &source, &box);
  return true;
}

bool D3d12Device::present(unsigned width, unsigned rows) {
  Shown* shown = shownForThisFrame(width, rows);
  if (shown == nullptr) return false;
  ID3D12GraphicsCommandList* list = shown->list;
  transition(list, texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list, shown->texture, frontend_.required_state, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION source = textureLevel(texture_);
  const D3D12_TEXTURE_COPY_LOCATION target = textureLevel(shown->texture);
  const D3D12_BOX box{0u, 0u, 0u, width, rows, 1u};
  list->CopyTextureRegion(&target, 0u, 0u, 0u, &source, &box);
  transition(list, shown->texture, D3D12_RESOURCE_STATE_COPY_DEST, frontend_.required_state);
  transition(list, texture_, D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (FAILED(list->Close())) return false;
  shown->fenceValue = execute(list);
  frontend_.set_texture(frontend_.handle, shown->texture, kTextureFormat);
  return true;
}

bool D3d12Device::presentPixels(const std::uint32_t* first, unsigned width, unsigned rows,
                                std::size_t pitch) {
  Shown* shown = shownForThisFrame(width, rows);
  if (shown == nullptr) return false;
  // Each XRGB8888 word's red and blue swapped into the texture's byte order, the spare byte made
  // opaque.
  auto* to = static_cast<std::uint8_t*>(shown->staging.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    const std::uint32_t* from = first + static_cast<std::size_t>(row) * pitch;
    auto* line = reinterpret_cast<std::uint32_t*>(to + static_cast<std::size_t>(row) * kRowPitch);
    for (unsigned x = 0; x < width; ++x) line[x] = texelOf(from[x]);
  }
  ID3D12GraphicsCommandList* list = shown->list;
  transition(list, shown->texture, frontend_.required_state, D3D12_RESOURCE_STATE_COPY_DEST);
  (void)copyRows(list, shown->staging.resource, shown->texture, true, width, rows);
  transition(list, shown->texture, D3D12_RESOURCE_STATE_COPY_DEST, frontend_.required_state);
  if (FAILED(list->Close())) return false;
  shown->fenceValue = execute(list);
  frontend_.set_texture(frontend_.handle, shown->texture, kTextureFormat);
  return true;
}

bool D3d12Device::readBack(unsigned width, unsigned rows, std::uint32_t* first,
                           std::size_t pitch) {
  const std::size_t bytes = kRowPitch * rows;
  if (bytes > readback_.size) {
    freeBuffer(readback_);
    if (!makeBuffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, readback_)) {
      freeBuffer(readback_);
      return false;
    }
  }
  waitFor(drawFenceValue_);
  if (FAILED(drawAllocator_->Reset()) || FAILED(drawList_->Reset(drawAllocator_, nullptr))) {
    return false;
  }
  transition(drawList_, texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  (void)copyRows(drawList_, texture_, readback_.resource, false, width, rows);
  transition(drawList_, texture_, D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!runOnce()) return false;
  const auto* texel = static_cast<const std::uint8_t*>(readback_.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    std::uint32_t* to = first + static_cast<std::size_t>(row) * pitch;
    const std::uint8_t* line = texel + static_cast<std::size_t>(row) * kRowPitch;
    for (unsigned x = 0; x < width; ++x) to[x] = wordOf(line + static_cast<std::size_t>(x) * kTexelBytes);
  }
  return true;
}

bool D3d12Device::seed(const std::uint32_t* first, unsigned width, unsigned rows,
                       std::size_t pitch) {
  const std::size_t bytes = kRowPitch * rows;
  if (bytes > upload_.size) {
    freeBuffer(upload_);
    if (!makeBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                    D3D12_RESOURCE_FLAG_NONE, upload_)) {
      freeBuffer(upload_);
      return false;
    }
  }
  waitFor(drawFenceValue_);
  auto* to = static_cast<std::uint8_t*>(upload_.mapped);
  for (unsigned row = 0; row < rows; ++row) {
    const std::uint32_t* from = first + static_cast<std::size_t>(row) * pitch;
    auto* line = reinterpret_cast<std::uint32_t*>(to + static_cast<std::size_t>(row) * kRowPitch);
    for (unsigned x = 0; x < width; ++x) line[x] = texelOf(from[x]);
  }
  if (FAILED(drawAllocator_->Reset()) || FAILED(drawList_->Reset(drawAllocator_, nullptr))) {
    return false;
  }
  transition(drawList_, texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_DEST);
  (void)copyRows(drawList_, upload_.resource, texture_, true, width, rows);
  transition(drawList_, texture_, D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!runOnce()) return false;
  shapeWidth_ = width;
  shapeRows_ = rows;
  return true;
}

}  // namespace snaggletooth_libretro
