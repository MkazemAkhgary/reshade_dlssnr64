#include "gpu_readback.h"
#include "profiling/profile.h"

namespace reshade_dlssnr {

bool GpuReadback::Ensure(ID3D12Device* device, size_t bytes_per_slot) {
  DLSSNR_PROFILE_RESOURCE_L2("GpuReadback::Ensure");
  if (device == nullptr || bytes_per_slot == 0) return false;
  if (device_.Get() == device && bytes_per_slot_ == bytes_per_slot) return true;

  Shutdown();

  for (uint32_t i = 0; i < kRingSize; ++i) {
    if (!slots_[i].buffer.Ensure(device, bytes_per_slot,
                                 D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_FLAG_NONE, "readback")) {
      Shutdown();
      return false;
    }

    void* mapped = nullptr;
    // READBACK heaps can be persistently mapped. We never write from the CPU,
    // so the map range is a read-only hint.
    D3D12_RANGE read_range = {0, bytes_per_slot};
    if (FAILED(slots_[i].buffer.Get()->Map(0, &read_range, &mapped))) {
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
  write_sequence_ = 0;
  last_consumed_sequence_ = 0;
  return true;
}

void GpuReadback::Shutdown() {
  DLSSNR_PROFILE_RESOURCE_L2("GpuReadback::Shutdown");
  for (auto& slot : slots_) {
    if (slot.buffer != nullptr && slot.mapped != nullptr) {
      slot.buffer.Get()->Unmap(0, nullptr);
    }
    slot.mapped = nullptr;
    // The GPU may still be copying into this slot; Reset() retires it.
    slot.buffer.Reset();
    slot.fence_value = 0;
    slot.bytes = 0;
    slot.sequence = 0;
    slot.valid = false;
  }
  device_.Reset();
  bytes_per_slot_ = 0;
  next_write_ = 0;
  write_sequence_ = 0;
  last_consumed_sequence_ = 0;
}

bool GpuReadback::EnqueueCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    uint32_t source_bytes,
    ID3D12Fence* fence,
    uint64_t fence_value) {
  DLSSNR_PROFILE_ASYNC_L2("GpuReadback::EnqueueCopy");
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
  slot.sequence = ++write_sequence_;
  slot.valid = true;
  return true;
}

const void* GpuReadback::TryGet(ID3D12Fence* fence, uint32_t* out_bytes) {
  DLSSNR_PROFILE_ASYNC_L3("GpuReadback::TryGet");
  if (fence == nullptr || bytes_per_slot_ == 0) return nullptr;

  const uint64_t completed = fence->GetCompletedValue();

  // Pick the most recently enqueued completed sample. Multiple passes can
  // enqueue copies behind the same Present fence, so fence value alone is not
  // a total ordering; sequence preserves the newest sample in that case.
  const Slot* newest = nullptr;
  for (const Slot& slot : slots_) {
    if (!slot.valid || slot.sequence <= last_consumed_sequence_) continue;
    if (completed < slot.fence_value) continue;
    if (newest == nullptr || slot.sequence > newest->sequence) newest = &slot;
  }

  if (newest == nullptr) return nullptr;

  last_consumed_sequence_ = newest->sequence;
  if (out_bytes) *out_bytes = newest->bytes;
  return newest->mapped;
}

}  // namespace reshade_dlssnr
