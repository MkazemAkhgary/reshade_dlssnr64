#include "gpu_resources.h"
#include "profiling/profile.h"

#include <cassert>
#include <mutex>
#include <vector>

#include <reshade.hpp>

#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {

D3D12_RESOURCE_DESC Texture2DDesc(uint32_t width,
                                  uint32_t height,
                                  DXGI_FORMAT format,
                                  D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

bool CreateTexture2D(ID3D12Device* device,
                     const D3D12_RESOURCE_DESC& desc,
                     D3D12_RESOURCE_STATES initial_state,
                     ID3D12Resource** out_resource) {
  DLSSNR_PROFILE_RESOURCE_L3("CreateTexture2D");
  if (device == nullptr || out_resource == nullptr) return false;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
      IID_PPV_ARGS(out_resource)));
}

bool CreateBuffer(ID3D12Device* device,
                  uint64_t bytes,
                  D3D12_HEAP_TYPE heap_type,
                  D3D12_RESOURCE_STATES initial_state,
                  D3D12_RESOURCE_FLAGS flags,
                  ID3D12Resource** out_resource) {
  DLSSNR_PROFILE_RESOURCE_L3("CreateBuffer");
  if (device == nullptr || out_resource == nullptr || bytes == 0) return false;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heap_type;

  D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
  desc.Flags = flags;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
      IID_PPV_ARGS(out_resource)));
}

bool CreateUavTexture2D(ID3D12Device* device,
                        const D3D12_RESOURCE_DESC& desc,
                        ID3D12Resource** out_resource) {
  D3D12_RESOURCE_DESC resource_desc = desc;
  resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return CreateTexture2D(device, resource_desc,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_resource);
}

bool CreateUavBuffer(ID3D12Device* device,
                     uint64_t bytes,
                     ID3D12Resource** out_resource) {
  return CreateBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      out_resource);
}

// ---------------------------------------------------------------------------
// ResourceRetireQueue
// ---------------------------------------------------------------------------

namespace {

// A healthy frame retires zero to two resources. Anything past this means a
// drain site stopped running, which leaks VRAM silently - so make it loud.
constexpr size_t kRetireQueueWarnDepth = 64;

}  // namespace

ResourceRetireQueue& ResourceRetireQueue::Get() {
  // Deliberately leaked: the queue must outlive every static that retires into
  // it during destruction, so it can never be a function-local object.
  static ResourceRetireQueue* queue = new ResourceRetireQueue();
  return *queue;
}

void ResourceRetireQueue::Retire(ID3D12Resource* resource,
                                 uint64_t fence_value) {
  assert(resource != nullptr);
  if (resource == nullptr) return;

  std::lock_guard<std::mutex> lock(lock_);
  entries_.push_back({resource, fence_value});

  // A missed drain is this queue's only silent failure mode: entries pile up
  // forever, nothing crashes, and VRAM just disappears.
  if (!depth_warned_ && entries_.size() > kRetireQueueWarnDepth) {
    depth_warned_ = true;
    reshade::log::message(
        reshade::log::level::warning,
        "DLSSNR: retire queue exceeded 64 entries; a drain site is probably "
        "not running, which leaks GPU memory");
  }
}

void ResourceRetireQueue::TagAll(uint64_t fence_value) {
  DLSSNR_PROFILE_ASYNC_L2("ResourceRetireQueue::TagAll");
  if (fence_value == 0) return;
  std::lock_guard<std::mutex> lock(lock_);
  for (Entry& entry : entries_) {
    if (entry.fence_value == 0) entry.fence_value = fence_value;
  }
}

size_t ResourceRetireQueue::ReleaseCompleted(uint64_t completed_fence_value) {
  std::lock_guard<std::mutex> lock(lock_);
  // A fence that goes backwards means a caller passed a stale or garbage
  // value. That leaks entries instead of failing, so assert on it.
  assert(completed_fence_value >= last_completed_fence_);
  last_completed_fence_ = completed_fence_value;

  size_t released = 0;
  auto it = entries_.begin();
  while (it != entries_.end()) {
    if (it->fence_value != 0 && it->fence_value <= completed_fence_value) {
      it->resource->Release();
      it = entries_.erase(it);
      ++released;
    } else {
      ++it;
    }
  }
  if (released != 0) depth_warned_ = false;
  return released;
}

void ResourceRetireQueue::ReleaseAllAfterGpuIdle() {
  std::lock_guard<std::mutex> lock(lock_);
  for (const Entry& entry : entries_) entry.resource->Release();
  entries_.clear();
  depth_warned_ = false;
  // The fence is replaced by the caller, so the next epoch starts at zero.
  last_completed_fence_ = 0;
}

bool ResourceRetireQueue::HasUntagged() const {
  std::lock_guard<std::mutex> lock(lock_);
  for (const Entry& entry : entries_) {
    if (entry.fence_value == 0) return true;
  }
  return false;
}

size_t ResourceRetireQueue::Count() const {
  std::lock_guard<std::mutex> lock(lock_);
  return entries_.size();
}

// ---------------------------------------------------------------------------
// OwnedTexture
// ---------------------------------------------------------------------------

bool OwnedTexture::Ensure(ID3D12Device* device,
                          const D3D12_RESOURCE_DESC& desc,
                          D3D12_RESOURCE_STATES initial_state,
                          const char* name) {
  DLSSNR_PROFILE_RESOURCE_L2("OwnedTexture::Ensure");
  if (device == nullptr) return false;
  if (Matches(device, desc.Width, desc.Height, desc.Format)) return true;

  Reset();

  ID3D12Resource* raw = nullptr;
  if (!CreateTexture2D(device, desc, initial_state, &raw)) return false;
  resource_.Attach(raw);
  device_ = device;
  width_ = static_cast<uint32_t>(desc.Width);
  height_ = static_cast<uint32_t>(desc.Height);
  format_ = desc.Format;
  name_ = name != nullptr ? name : "";
  TRACE_NAME(resource_.Get(), name_);
  TRACE_CMD(this, "create %s res=%d %ux%u fmt=%d", name_,
            trace::ResourceId(resource_.Get()), width_, height_,
            static_cast<int>(format_));
  return true;
}

bool OwnedTexture::EnsureUav(ID3D12Device* device,
                             const D3D12_RESOURCE_DESC& desc,
                             const char* name) {
  D3D12_RESOURCE_DESC uav_desc = desc;
  uav_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return Ensure(device, uav_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
}

void OwnedTexture::Reset() {
  if (resource_ == nullptr) return;
  // Never release inline: an earlier frame's command list may still read it.
  ResourceRetireQueue::Get().Retire(resource_.Detach());
  device_.Reset();
  width_ = 0;
  height_ = 0;
  format_ = DXGI_FORMAT_UNKNOWN;
  name_ = "";
}

// ---------------------------------------------------------------------------
// OwnedBuffer
// ---------------------------------------------------------------------------

bool OwnedBuffer::Ensure(ID3D12Device* device,
                         uint64_t bytes,
                         D3D12_HEAP_TYPE heap_type,
                         D3D12_RESOURCE_STATES initial_state,
                         D3D12_RESOURCE_FLAGS flags,
                         const char* name) {
  DLSSNR_PROFILE_RESOURCE_L2("OwnedBuffer::Ensure");
  if (device == nullptr) return false;
  if (Matches(device, bytes)) return true;

  Reset();

  ID3D12Resource* raw = nullptr;
  if (!CreateBuffer(device, bytes, heap_type, initial_state, flags, &raw)) {
    return false;
  }
  resource_.Attach(raw);
  device_ = device;
  bytes_ = bytes;
  name_ = name != nullptr ? name : "";
  TRACE_NAME(resource_.Get(), name_);
  TRACE_CMD(this, "create %s res=%d bytes=%llu", name_,
            trace::ResourceId(resource_.Get()),
            static_cast<unsigned long long>(bytes_));
  return true;
}

bool OwnedBuffer::EnsureUav(ID3D12Device* device,
                            uint64_t bytes,
                            const char* name) {
  return Ensure(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, name);
}

void OwnedBuffer::Reset() {
  if (resource_ == nullptr) return;
  ResourceRetireQueue::Get().Retire(resource_.Detach());
  device_.Reset();
  bytes_ = 0;
  name_ = "";
}

// ---------------------------------------------------------------------------
// BorrowedRef
// ---------------------------------------------------------------------------

BorrowedRef BorrowedRef::AddRef(ID3D12Resource* resource) {
  BorrowedRef ref;
  if (resource == nullptr) return ref;
  resource->AddRef();
  ref.resource_ = resource;
  return ref;
}

void BorrowedRef::Reset(uint64_t fence_value) {
  if (resource_ == nullptr) return;
  ResourceRetireQueue::Get().Retire(resource_, fence_value);
  resource_ = nullptr;
}

}  // namespace reshade_dlssnr
