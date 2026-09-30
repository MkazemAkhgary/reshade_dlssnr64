#include "gpu_readback.h"

namespace reshade_dlssnr {

bool GpuReadback::Ensure(ID3D12Device* device, size_t bytes_per_slot) {
  if (device == nullptr || bytes_per_slot == 0) return false;
  if (device_.Get() == device && bytes_per_slot_ == bytes_per_slot) return true;

  Shutdown();

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = bytes_per_slot;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  for (uint32_t i = 0; i < kRingSize; ++i) {
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(slots_[i].buffer.GetAddressOf())))) {
      Shutdown();
      return false;
    }

    void* mapped = nullptr;
    // READBACK heaps can be persistently mapped. We never write from the CPU,
    // so the map range is a read-only hint.
    D3D12_RANGE read_range = {0, bytes_per_slot};
    if (FAILED(slots_[i].buffer->Map(0, &read_range, &mapped))) {
      Shutdown();
      return false;
    }
    slots_[i].mapped = mapped;
    slots_[i].bytes = 0;
    slots_[i].valid = false;
  }

  device_ = device;
  bytes_per_slot_ = bytes_per_slot;
  next_write_ = 0;
  last_consumed_fence_ = 0;
  return true;
}

void GpuReadback::Shutdown() {
  for (auto& slot : slots_) {
    if (slot.buffer != nullptr && slot.mapped != nullptr) {
      slot.buffer->Unmap(0, nullptr);
    }
    slot.mapped = nullptr;
    slot.buffer.Reset();
    slot.fence_value = 0;
    slot.bytes = 0;
    slot.valid = false;
  }
  device_.Reset();
  bytes_per_slot_ = 0;
  next_write_ = 0;
  last_consumed_fence_ = 0;
}

bool GpuReadback::EnqueueCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    uint32_t source_bytes,
    ID3D12Fence* fence,
    uint64_t fence_value) {
  if (command_list == nullptr || source == nullptr || fence == nullptr ||
      bytes_per_slot_ == 0) {
    return false;
  }
  if (source_bytes == 0 || source_bytes > bytes_per_slot_) {
    return false;
  }

  Slot& slot = slots_[next_write_ % kRingSize];
  next_write_++;

  // If the slot we're about to reuse hasn't retired yet, skip this frame.
  if (slot.valid && fence->GetCompletedValue() < slot.fence_value) {
    return false;
  }

  command_list->CopyBufferRegion(
      slot.buffer.Get(), 0, source, 0, source_bytes);

  slot.fence_value = fence_value;
  slot.bytes = source_bytes;
  slot.valid = true;
  return true;
}

const void* GpuReadback::TryGet(ID3D12Fence* fence, uint32_t* out_bytes) {
  if (fence == nullptr || bytes_per_slot_ == 0) return nullptr;

  const uint64_t completed = fence->GetCompletedValue();

  // Find the slot with the highest fence value that is both newer than what
  // we already returned and known to have completed. Newer so the caller
  // gets each frame's data at most once.
  const Slot* newest = nullptr;
  for (const Slot& slot : slots_) {
    if (!slot.valid) continue;
    if (slot.fence_value <= last_consumed_fence_) continue;
    if (completed < slot.fence_value) continue;
    if (newest == nullptr || slot.fence_value > newest->fence_value) {
      newest = &slot;
    }
  }

  if (newest == nullptr) return nullptr;

  last_consumed_fence_ = newest->fence_value;
  if (out_bytes) *out_bytes = newest->bytes;
  return newest->mapped;
}

}  // namespace reshade_dlssnr
